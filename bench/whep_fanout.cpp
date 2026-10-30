#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <string_view>
#include <sys/resource.h>
#include <condition_variable>

#include <boost/asio/post.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "bench/packet_health.h"
#include "bench/credential_file.h"
#include "bench/measurement_start.h"
#include "bench/clients/webrtc_client.h"

namespace
{

using clock_type = std::chrono::steady_clock;

constexpr std::uint64_t video_clock_khz = 90;

struct configuration
{
    std::string whep_url;
    std::filesystem::path whep_urls_file;
    std::vector<std::string> whep_urls;
    std::string start_file;
    bool video_only{};
    std::chrono::seconds ready_timeout{60};
    std::chrono::seconds progress_interval{5};
    boost::asio::ip::address bind_address{boost::asio::ip::address_v4::any()};
    std::size_t viewers{};
    std::size_t sources{1};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::chrono::seconds warmup{5};
    std::chrono::seconds duration{15};
    std::filesystem::path dump_file;
    std::string opus_format_parameters{"minptime=10;useinbandfec=1;stereo=1"};
};

struct process_sample
{
    double cpu_seconds{};
    std::uint64_t rss_kib{};
    std::uint64_t pss_kib{};
    std::size_t fds{};
};

struct shared_results
{
    std::atomic_size_t ready{};
    std::atomic_size_t media_ready{};
    std::atomic_size_t establishment_failures{};
    std::atomic_size_t runtime_failures{};
    std::atomic_size_t stopped{};
    std::atomic_bool measuring{};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> errors;
    std::vector<std::uint64_t> first_media_milliseconds;

    void add_error(std::string value)
    {
        std::lock_guard lock(mutex);
        if (errors.size() < 20U)
        {
            errors.push_back(std::move(value));
        }
    }
};

std::string argument_value(int& index, int argc, char** argv, std::string_view name)
{
    if (index + 1 >= argc)
    {
        throw std::runtime_error("missing argument " + std::string(name));
    }
    return argv[++index];
}

configuration parse_arguments(int argc, char** argv)
{
    configuration config;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--whep-url")
        {
            config.whep_url = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--whep-urls-file")
        {
            config.whep_urls_file = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--start-file")
        {
            config.start_file = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--video-only")
        {
            config.video_only = true;
        }
        else if (argument == "--ready-timeout")
        {
            config.ready_timeout = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
        }
        else if (argument == "--progress-interval")
        {
            config.progress_interval = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
        }
        else if (argument == "--bind-address")
        {
            config.bind_address = boost::asio::ip::make_address(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--viewers")
        {
            config.viewers = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--sources")
        {
            config.sources = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--ramp-per-second")
        {
            config.ramp_per_second = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--io-threads")
        {
            config.io_threads = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--duration")
        {
            config.duration = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
        }
        else if (argument == "--warmup")
        {
            config.warmup = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
        }
        else if (argument == "--dump-file")
        {
            config.dump_file = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--opus-fmtp")
        {
            config.opus_format_parameters = argument_value(index, argc, argv, argument);
        }
        else
        {
            throw std::runtime_error("unknown argument " + std::string(argument));
        }
    }
    if (config.whep_url.empty() == config.whep_urls_file.empty() || config.viewers == 0U || config.sources == 0U ||
        (config.whep_urls_file.empty() && config.sources > 1U && !config.whep_url.ends_with("perf0")) || config.ramp_per_second == 0U ||
        config.io_threads == 0U || config.io_threads > 64U || config.duration.count() <= 0 || config.warmup.count() < 0 ||
        config.ready_timeout.count() <= 0 || config.progress_interval.count() <= 0)
    {
        throw std::runtime_error(
            "exactly one of whep-url or whep-urls-file, viewers, ramp-per-second, io-threads and positive duration are required");
    }
    if (!config.whep_urls_file.empty())
    {
        config.whep_urls = media_server::bench::read_credentials(config.whep_urls_file.string(), config.viewers);
    }
    return config;
}

std::uint64_t read_kib_value(const std::filesystem::path& path, std::string_view key)
{
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.starts_with(key))
        {
            std::istringstream values(line.substr(key.size()));
            std::uint64_t value{};
            values >> value;
            return value;
        }
    }
    return 0;
}

process_sample sample_process()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        throw std::runtime_error("getrusage failed");
    }
    const auto seconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1'000'000.0;
    std::size_t fds{};
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        (void)entry;
        ++fds;
    }
    return {seconds, read_kib_value("/proc/self/status", "VmRSS:"), read_kib_value("/proc/self/smaps_rollup", "Pss:"), fds};
}

std::uint32_t read_u32(std::span<const std::uint8_t> packet, std::size_t offset)
{
    return (static_cast<std::uint32_t>(packet[offset]) << 24U) | (static_cast<std::uint32_t>(packet[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(packet[offset + 2U]) << 8U) | packet[offset + 3U];
}

class play_session final : public std::enable_shared_from_this<play_session>
{
   public:
    play_session(std::shared_ptr<shared_results> results,
                 std::shared_ptr<media_server::bench::webrtc_client_peer> peer,
                 std::string resource_url,
                 std::size_t index,
                 bool expect_audio,
                 clock_type::time_point started,
                 std::shared_ptr<std::ofstream> dump)
        : results_(std::move(results)),
          peer_(std::move(peer)),
          resource_url_(std::move(resource_url)),
          index_(index),
          expect_audio_(expect_audio),
          started_(started),
          dump_(std::move(dump))
    {
    }

    void start()
    {
        auto self = shared_from_this();
        peer_->start_receive([self](bool rtcp, std::span<const std::uint8_t> packet) { self->on_packet(rtcp, packet); },
                             [self](const boost::system::error_code& error)
                             {
                                 if (!self->intentional_stop_)
                                 {
                                     self->fail("udp_receive", error.message());
                                 }
                             });
    }

    void stop()
    {
        if (terminal_)
        {
            return;
        }
        terminal_ = true;
        intentional_stop_ = true;
        peer_->close();
        ++results_->stopped;
        results_->changed.notify_all();
    }

    [[nodiscard]] std::uint64_t video_packets() const noexcept { return video_packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t audio_packets() const noexcept { return audio_packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t video_media_ms() const noexcept { return video_media_ticks_.load(std::memory_order_relaxed) / video_clock_khz; }
    [[nodiscard]] std::int64_t lag_span_ms() const noexcept
    {
        const auto minimum = lag_min_.load(std::memory_order_relaxed);
        const auto maximum = lag_max_.load(std::memory_order_relaxed);
        return maximum >= minimum ? maximum - minimum : -1;
    }
    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return received_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t received_packets() const noexcept { return received_packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t received_media_datagrams() const noexcept { return peer_->received_media_datagrams(); }
    [[nodiscard]] std::uint64_t unprotect_failures() const noexcept { return peer_->unprotect_failures(); }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] const std::string& resource_url() const noexcept { return resource_url_; }

   private:
    void on_packet(bool rtcp, std::span<const std::uint8_t> packet)
    {
        received_bytes_.fetch_add(packet.size(), std::memory_order_relaxed);
        received_packets_.fetch_add(1U, std::memory_order_relaxed);
        if (dump_)
        {
            const auto nanoseconds =
                static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - started_).count());
            const auto kind = static_cast<std::uint8_t>(rtcp);
            const auto size = static_cast<std::uint16_t>(packet.size());
            dump_->write(reinterpret_cast<const char*>(&nanoseconds), sizeof(nanoseconds));
            dump_->write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            dump_->write(reinterpret_cast<const char*>(&size), sizeof(size));
            dump_->write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
        }
        if (rtcp || packet.size() < 12U)
        {
            return;
        }
        const auto payload_type = static_cast<std::uint8_t>(packet[1] & 0x7fU);
        if (payload_type == media_server::bench::webrtc_h264_payload_type || payload_type == media_server::bench::webrtc_h264_main_payload_type ||
            payload_type == media_server::bench::webrtc_h264_high_payload_type || payload_type == media_server::bench::webrtc_h265_payload_type)
        {
            if (!video_health_.observe(packet))
            {
                fail("rtp_health", "video sequence gap or timestamp reversal");
                return;
            }
            const auto media_ticks = video_health_.advanced();
            if (results_->measuring.load(std::memory_order_relaxed) && media_ticks != video_media_ticks_.load(std::memory_order_relaxed))
            {
                const auto lag = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now().time_since_epoch()).count() -
                                 static_cast<std::int64_t>(media_ticks / video_clock_khz);
                lag_min_.store(std::min(lag_min_.load(std::memory_order_relaxed), lag), std::memory_order_relaxed);
                lag_max_.store(std::max(lag_max_.load(std::memory_order_relaxed), lag), std::memory_order_relaxed);
            }
            video_media_ticks_.store(media_ticks, std::memory_order_relaxed);
            ++video_packets_;
            if (!pli_sent_)
            {
                const auto media_ssrc = read_u32(packet, 8U);
                const auto sender_ssrc = 0x30000000U + static_cast<std::uint32_t>(index_);
                const std::array<std::uint8_t, 12> pli{
                    0x81,
                    206,
                    0,
                    2,
                    static_cast<std::uint8_t>(sender_ssrc >> 24U),
                    static_cast<std::uint8_t>(sender_ssrc >> 16U),
                    static_cast<std::uint8_t>(sender_ssrc >> 8U),
                    static_cast<std::uint8_t>(sender_ssrc),
                    static_cast<std::uint8_t>(media_ssrc >> 24U),
                    static_cast<std::uint8_t>(media_ssrc >> 16U),
                    static_cast<std::uint8_t>(media_ssrc >> 8U),
                    static_cast<std::uint8_t>(media_ssrc),
                };
                pli_sent_ = peer_->send_rtcp(pli);
            }
        }
        else if (payload_type == media_server::bench::webrtc_opus_payload_type || payload_type == media_server::bench::webrtc_pcma_payload_type ||
                 payload_type == media_server::bench::webrtc_pcmu_payload_type)
        {
            if (!audio_health_.observe(packet))
            {
                fail("rtp_health", "audio sequence gap or timestamp reversal");
                return;
            }
            ++audio_packets_;
        }
        if (!media_ready_ && video_packets_.load() != 0U && (!expect_audio_ || audio_packets_.load() != 0U))
        {
            media_ready_ = true;
            {
                std::lock_guard lock(results_->mutex);
                results_->first_media_milliseconds.push_back(
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - started_).count()));
                ++results_->media_ready;
            }
            results_->changed.notify_all();
        }
    }

    void fail(std::string_view phase, std::string detail)
    {
        if (terminal_)
        {
            return;
        }
        terminal_ = true;
        ++results_->runtime_failures;
        results_->add_error("viewer=" + std::to_string(index_) + " phase=" + std::string(phase) + " error=" + std::move(detail));
        peer_->close();
        ++results_->stopped;
        results_->changed.notify_all();
    }

   private:
    std::shared_ptr<shared_results> results_;
    std::shared_ptr<media_server::bench::webrtc_client_peer> peer_;
    std::string resource_url_;
    std::size_t index_{};
    bool expect_audio_{};
    clock_type::time_point started_;
    std::shared_ptr<std::ofstream> dump_;
    std::atomic_uint64_t video_packets_{};
    std::atomic_uint64_t audio_packets_{};
    std::atomic_uint64_t received_bytes_{};
    std::atomic_uint64_t received_packets_{};
    std::atomic_uint64_t video_media_ticks_{};
    std::atomic_int64_t lag_min_{std::numeric_limits<std::int64_t>::max()};
    std::atomic_int64_t lag_max_{std::numeric_limits<std::int64_t>::min()};
    media_server::bench::rtp_health video_health_{media_server::bench::video_reorder_ticks};
    media_server::bench::rtp_health audio_health_;
    bool pli_sent_{};
    bool media_ready_{};
    bool intentional_stop_{};
    bool terminal_{};
};

struct io_shard
{
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::jthread thread;
};

void stop_shards(std::vector<std::unique_ptr<io_shard>>& shards)
{
    for (auto& shard : shards)
    {
        shard->work.reset();
        shard->io.stop();
    }
    for (auto& shard : shards)
    {
        shard->thread.join();
    }
}

struct media_totals
{
    std::uint64_t bytes{};
    std::uint64_t packets{};
    std::uint64_t video{};
    std::uint64_t audio{};
    std::uint64_t media_datagrams{};
    std::uint64_t unprotect_failures{};
};

media_totals total_media(const std::vector<std::shared_ptr<play_session>>& sessions)
{
    media_totals result;
    for (const auto& session : sessions)
    {
        result.bytes += session->received_bytes();
        result.packets += session->received_packets();
        result.video += session->video_packets();
        result.audio += session->audio_packets();
        result.media_datagrams += session->received_media_datagrams();
        result.unprotect_failures += session->unprotect_failures();
    }
    return result;
}

std::size_t stop_and_remove_sessions(const std::vector<std::shared_ptr<play_session>>& sessions,
                                     const std::shared_ptr<shared_results>& results,
                                     std::vector<std::unique_ptr<io_shard>>& shards)
{
    for (const auto& session : sessions)
    {
        boost::asio::post(shards[session->index() % shards.size()]->io, [session]() { session->stop(); });
    }
    {
        std::unique_lock lock(results->mutex);
        results->changed.wait_for(lock, std::chrono::seconds(5), [&]() { return results->stopped.load() == sessions.size(); });
    }
    std::size_t removed{};
    for (const auto& session : sessions)
    {
        std::string error;
        if (media_server::bench::delete_webrtc_resource(session->resource_url(), error))
        {
            ++removed;
        }
        else
        {
            results->add_error("WHEP DELETE: " + error);
        }
    }
    return removed;
}

}    // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "--whep-urls-file FILE | --whep-url URL\n"
                     "--viewers N --sources N --bind-address ADDRESS --io-threads N\n"
                     "--ramp-per-second N --ready-timeout SECONDS --video-only\n"
                     "--warmup SECONDS --duration SECONDS --progress-interval SECONDS --start-file FILE\n"
                     "--dump-file FILE (first viewer's decrypted RTP/RTCP: int64 ns, uint8 rtcp, uint16 size, bytes)\n"
                     "--opus-fmtp PARAMETERS (offered Opus fmtp, default minptime=10;useinbandfec=1;stereo=1)\n";
        return 0;
    }
    try
    {
        const auto config = parse_arguments(argc, argv);
        auto context = media_server::bench::webrtc_client_context::create();
        if (!context)
        {
            throw std::runtime_error("failed to create WebRTC client context");
        }
        auto results = std::make_shared<shared_results>();
        std::vector<std::unique_ptr<io_shard>> shards;
        for (std::size_t index = 0; index < config.io_threads; ++index)
        {
            auto shard = std::make_unique<io_shard>();
            shard->thread = std::jthread(
                [pointer = shard.get()](std::stop_token stop_token)
                {
                    std::stop_callback stop_callback(stop_token, [pointer]() { pointer->io.stop(); });
                    pointer->io.run();
                });
            shards.emplace_back(std::move(shard));
        }

        std::vector<std::shared_ptr<play_session>> sessions;
        sessions.reserve(config.viewers);
        std::vector<std::uint64_t> post_milliseconds;
        std::vector<std::uint64_t> establish_milliseconds;
        const auto offer = context->make_offer(media_server::bench::webrtc_client_direction::play, config.opus_format_parameters);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.viewers; ++index)
        {
            const auto started = clock_type::now();
            auto& shard = *shards[index % shards.size()];
            auto peer = std::make_shared<media_server::bench::webrtc_client_peer>(shard.io, context, config.bind_address);
            media_server::bench::webrtc_http_response response;
            std::string error;
            const auto url = !config.whep_urls.empty() ? config.whep_urls[index]
                             : config.sources == 1U    ? config.whep_url
                                                    : config.whep_url.substr(0, config.whep_url.size() - 1U) + std::to_string(index % config.sources);
            const auto posted = media_server::bench::post_webrtc_offer(url, offer, response, error);
            const auto answered = clock_type::now();
            const auto connected = posted && response.status == 201U && peer->establish(response.body, error);
            if (!connected)
            {
                ++results->establishment_failures;
                results->add_error("viewer=" + std::to_string(index) +
                                   " setup=" + (error.empty() ? "HTTP " + std::to_string(response.status) + " " + response.body : error));
                if (!response.location.empty())
                {
                    std::string delete_error;
                    if (!media_server::bench::delete_webrtc_resource(response.location, delete_error))
                    {
                        results->add_error("WHEP DELETE after setup failure: " + delete_error);
                    }
                }
            }
            else
            {
                post_milliseconds.push_back(
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(answered - started).count()));
                establish_milliseconds.push_back(
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - answered).count()));
                auto dump = index == 0U && !config.dump_file.empty() ? std::make_shared<std::ofstream>(config.dump_file, std::ios::binary) : nullptr;
                auto session = std::make_shared<play_session>(results, peer, response.location, index, !config.video_only, started, std::move(dump));
                sessions.push_back(session);
                ++results->ready;
                boost::asio::post(shard.io, [session]() { session->start(); });
            }
            const auto next = ramp_started + ramp_interval * static_cast<std::int64_t>(index + 1U);
            if (next > clock_type::now())
            {
                std::this_thread::sleep_until(next);
            }
        }
        {
            std::unique_lock lock(results->mutex);
            results->changed.wait_for(
                lock, config.ready_timeout, [&]() { return results->media_ready.load() + results->establishment_failures.load() == config.viewers; });
        }

        const auto established = sample_process();
        std::vector<std::uint64_t> latencies;
        {
            std::lock_guard lock(results->mutex);
            latencies = results->first_media_milliseconds;
        }
        std::sort(latencies.begin(), latencies.end());
        if (!latencies.empty())
        {
            std::cout << "first_media_ms_p50=" << latencies[(latencies.size() - 1U) / 2U]
                      << " p95=" << latencies[(latencies.size() - 1U) * 95U / 100U] << " p99=" << latencies[(latencies.size() - 1U) * 99U / 100U]
                      << " max=" << latencies.back() << '\n';
        }
        const auto setup_percentile = [](std::vector<std::uint64_t>& values, std::size_t percent)
        {
            std::sort(values.begin(), values.end());
            return values.empty() ? 0U : values[(values.size() - 1U) * percent / 100U];
        };
        std::cout << "phase=setup post_ms_p50=" << setup_percentile(post_milliseconds, 50)
                  << " post_ms_p90=" << setup_percentile(post_milliseconds, 90) << " post_ms_p99=" << setup_percentile(post_milliseconds, 99)
                  << " post_ms_max=" << setup_percentile(post_milliseconds, 100)
                  << " establish_ms_p50=" << setup_percentile(establish_milliseconds, 50)
                  << " establish_ms_p90=" << setup_percentile(establish_milliseconds, 90)
                  << " establish_ms_p99=" << setup_percentile(establish_milliseconds, 99)
                  << " establish_ms_max=" << setup_percentile(establish_milliseconds, 100) << '\n';
        std::cout << "phase=established requested=" << config.viewers << " ready=" << results->ready.load()
                  << " media_ready=" << results->media_ready.load() << " establishment_failures=" << results->establishment_failures.load()
                  << " ramp_seconds=" << std::chrono::duration<double>(clock_type::now() - ramp_started).count()
                  << " generator_cpu_seconds=" << established.cpu_seconds << " generator_rss_kib=" << established.rss_kib
                  << " generator_pss_kib=" << established.pss_kib << " generator_fds=" << established.fds << std::endl;
        if (results->media_ready.load() != config.viewers)
        {
            std::size_t missing_reported{};
            for (const auto& session : sessions)
            {
                if ((session->video_packets() == 0U || (!config.video_only && session->audio_packets() == 0U)) && missing_reported++ < 20U)
                {
                    std::cerr << "viewer=" << session->index() << " media_not_ready video_packets=" << session->video_packets()
                              << " audio_packets=" << session->audio_packets() << " received_packets=" << session->received_packets()
                              << " received_media_datagrams=" << session->received_media_datagrams()
                              << " unprotect_failures=" << session->unprotect_failures() << '\n';
                }
            }
            {
                std::lock_guard lock(results->mutex);
                for (const auto& error : results->errors)
                {
                    std::cerr << error << '\n';
                }
            }
            const auto removed = stop_and_remove_sessions(sessions, results, shards);
            std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.viewers << '\n';
            stop_shards(shards);
            return 1;
        }

        std::cout << "phase=media_ready ready=" << results->media_ready.load() << std::endl;
        boost::asio::io_context barrier;
        std::exception_ptr barrier_error;
        clock_type::time_point requested_start;
        boost::asio::co_spawn(barrier,
                              media_server::bench::wait_measurement_start(barrier, config.start_file, config.warmup, config.ready_timeout),
                              [&](std::exception_ptr error, clock_type::time_point start)
                              {
                                  barrier_error = error;
                                  requested_start = start;
                              });
        barrier.run();
        if (barrier_error)
        {
            try
            {
                std::rethrow_exception(barrier_error);
            }
            catch (const std::exception& error)
            {
                std::cerr << "phase=measurement_barrier error=" << error.what() << '\n';
            }
            stop_and_remove_sessions(sessions, results, shards);
            stop_shards(shards);
            return 1;
        }
        const auto measurement_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
        std::cout << "phase=measurement_start monotonic_ns=" << measurement_start_ns << std::endl;
        const auto before = total_media(sessions);
        struct session_sample
        {
            std::uint64_t bytes{};
            std::uint64_t video{};
            std::uint64_t audio{};
            std::uint64_t media_ms{};
        };
        std::vector<session_sample> session_before;
        for (const auto& session : sessions)
        {
            session_before.push_back({session->received_bytes(), session->video_packets(), session->audio_packets(), session->video_media_ms()});
        }
        const auto process_before = sample_process();
        results->measuring = true;
        const auto started = clock_type::now();
        const auto deadline = requested_start + config.duration;
        bool all_windows_healthy = true;
        auto progress_before = session_before;
        auto progress_started = started;
        while (progress_started < deadline)
        {
            const bool full_window = deadline - progress_started >= config.progress_interval;
            std::this_thread::sleep_until(std::min(progress_started + config.progress_interval, deadline));
            const auto progress_finished = clock_type::now();
            std::size_t progress_count{};
            std::uint64_t progress_video_min = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t progress_audio_min = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t progress_bytes_min = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0; index < sessions.size(); ++index)
            {
                const session_sample current{sessions[index]->received_bytes(),
                                             sessions[index]->video_packets(),
                                             sessions[index]->audio_packets(),
                                             sessions[index]->video_media_ms()};
                const auto video = current.video - progress_before[index].video;
                const auto audio = current.audio - progress_before[index].audio;
                progress_count += video > 0 && (config.video_only || audio > 0) ? 1U : 0U;
                progress_video_min = std::min(progress_video_min, video);
                progress_audio_min = std::min(progress_audio_min, audio);
                progress_bytes_min = std::min(progress_bytes_min, current.bytes - progress_before[index].bytes);
                progress_before[index] = current;
            }
            if (deadline - progress_started >= std::chrono::seconds{1} && progress_count != config.viewers)
            {
                all_windows_healthy = false;
            }
            std::cout << "phase=progress progressing=" << progress_count << " full_window=" << full_window
                      << " duration_seconds=" << std::chrono::duration<double>(progress_finished - progress_started).count()
                      << " viewer_video_packets_min=" << progress_video_min << " viewer_audio_packets_min=" << progress_audio_min
                      << " viewer_received_bytes_min=" << progress_bytes_min << std::endl;
            progress_started = progress_finished;
        }
        results->measuring = false;
        const auto measurement_end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
        const auto elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        const auto process_after = sample_process();
        const auto after = total_media(sessions);
        std::size_t progressing{};
        std::vector<std::uint64_t> viewer_bytes;
        viewer_bytes.reserve(sessions.size());
        std::vector<double> viewer_realtime;
        viewer_realtime.reserve(sessions.size());
        std::vector<std::int64_t> viewer_lag_span;
        std::uint64_t min_video_packets = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t min_audio_packets = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            const auto video = sessions[index]->video_packets() - session_before[index].video;
            const auto audio = sessions[index]->audio_packets() - session_before[index].audio;
            viewer_bytes.push_back(sessions[index]->received_bytes() - session_before[index].bytes);
            viewer_realtime.push_back(static_cast<double>(sessions[index]->video_media_ms() - session_before[index].media_ms) / 1000.0 / elapsed);
            if (sessions[index]->lag_span_ms() >= 0)
            {
                viewer_lag_span.push_back(sessions[index]->lag_span_ms());
            }
            min_video_packets = std::min(min_video_packets, video);
            min_audio_packets = std::min(min_audio_packets, audio);
            progressing += video > 0 && (config.video_only || audio > 0) ? 1U : 0U;
        }
        std::ranges::sort(viewer_bytes);
        std::ranges::sort(viewer_realtime);
        std::ranges::sort(viewer_lag_span);
        const auto lag_span = [&](std::size_t percentile)
        { return viewer_lag_span.empty() ? 0 : viewer_lag_span[(viewer_lag_span.size() - 1U) * percentile / 100U]; };
        const auto realtime = [&](std::size_t percentile) { return viewer_realtime[(viewer_realtime.size() - 1U) * percentile / 100U]; };
        const auto viewer_rate = [&](std::size_t percentile)
        { return static_cast<double>(viewer_bytes[(viewer_bytes.size() - 1U) * percentile / 100U]) / elapsed; };
        std::cout << "phase=measurement requested=" << config.viewers << " ready=" << results->media_ready.load()
                  << " measurement_start_monotonic_ns=" << measurement_start_ns << " measurement_end_monotonic_ns=" << measurement_end_ns
                  << " measurement_started=1 client_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " steady_media_bytes=" << after.bytes - before.bytes << " progressing=" << progressing
                  << " runtime_failures=" << results->runtime_failures.load() << " duration_seconds=" << elapsed
                  << " received_bytes=" << after.bytes - before.bytes << " received_packets=" << after.packets - before.packets
                  << " video_packets=" << after.video - before.video << " audio_packets=" << after.audio - before.audio
                  << " media_datagrams=" << after.media_datagrams - before.media_datagrams << " unprotect_failures=" << after.unprotect_failures
                  << " bytes_per_viewer_second=" << static_cast<double>(after.bytes - before.bytes) / elapsed / static_cast<double>(config.viewers)
                  << " viewer_bytes_per_second_min=" << viewer_rate(0) << " viewer_bytes_per_second_p10=" << viewer_rate(10)
                  << " viewer_bytes_per_second_p50=" << viewer_rate(50) << " viewer_bytes_per_second_p90=" << viewer_rate(90)
                  << " viewer_bytes_per_second_max=" << viewer_rate(100) << " viewer_video_packets_min=" << min_video_packets
                  << " viewer_audio_packets_min=" << min_audio_packets << " viewer_realtime_min=" << realtime(0)
                  << " viewer_realtime_p10=" << realtime(10) << " viewer_realtime_p50=" << realtime(50) << " viewer_realtime_max=" << realtime(100)
                  << " viewer_lag_span_ms_p50=" << lag_span(50) << " viewer_lag_span_ms_p90=" << lag_span(90)
                  << " viewer_lag_span_ms_max=" << lag_span(100)
                  << " generator_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " generator_rss_kib=" << process_after.rss_kib << " generator_pss_kib=" << process_after.pss_kib
                  << " generator_fds=" << process_after.fds << std::endl;

        const auto removed = stop_and_remove_sessions(sessions, results, shards);
        std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.viewers << '\n';
        stop_shards(shards);
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
        return all_windows_healthy && progressing == config.viewers && results->runtime_failures.load() == 0U &&
                       results->stopped.load() == config.viewers && removed == config.viewers
                   ? 0
                   : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fanout WHEP player failed: " << error.what() << '\n';
        return 2;
    }
}
