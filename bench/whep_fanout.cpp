#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include "bench/clients/webrtc_client.h"

namespace
{

using clock_type = std::chrono::steady_clock;

struct configuration
{
    std::string whep_url;
    std::size_t viewers{};
    std::size_t sources{1};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::chrono::seconds warmup{5};
    std::chrono::seconds duration{15};
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
        else
        {
            throw std::runtime_error("unknown argument " + std::string(argument));
        }
    }
    if (config.whep_url.empty() || config.viewers == 0U || config.sources == 0U ||
        (config.sources > 1U && !config.whep_url.ends_with("perf0")) || config.ramp_per_second == 0U ||
        config.io_threads == 0U || config.io_threads > 64U || config.duration.count() <= 0 || config.warmup.count() < 0)
    {
        throw std::runtime_error("whep-url, viewers, ramp-per-second, io-threads and positive duration are required");
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
                 std::uint8_t video_payload_type,
                 clock_type::time_point started)
        : results_(std::move(results)), peer_(std::move(peer)), resource_url_(std::move(resource_url)), index_(index),
          video_payload_type_(video_payload_type), started_(started)
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
        if (rtcp || packet.size() < 12U)
        {
            return;
        }
        const auto payload_type = static_cast<std::uint8_t>(packet[1] & 0x7fU);
        if (payload_type == video_payload_type_)
        {
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
        else if (payload_type == 111U)
        {
            ++audio_packets_;
        }
        if (!media_ready_ && video_packets_.load() != 0U && audio_packets_.load() != 0U)
        {
            media_ready_ = true;
            {
                std::lock_guard lock(results_->mutex);
                results_->first_media_milliseconds.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - started_).count()));
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
    std::uint8_t video_payload_type_{};
    clock_type::time_point started_;
    std::atomic_uint64_t video_packets_{};
    std::atomic_uint64_t audio_packets_{};
    std::atomic_uint64_t received_bytes_{};
    std::atomic_uint64_t received_packets_{};
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
        const auto offer = context->make_offer(media_server::bench::webrtc_client_direction::play);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.viewers; ++index)
        {
            const auto started = clock_type::now();
            auto& shard = *shards[index % shards.size()];
            auto peer = std::make_shared<media_server::bench::webrtc_client_peer>(shard.io, context);
            media_server::bench::webrtc_http_response response;
            std::string error;
            const auto url = config.sources == 1U
                                 ? config.whep_url
                                 : config.whep_url.substr(0, config.whep_url.size() - 1U) + std::to_string(index % config.sources);
            const auto posted = media_server::bench::post_webrtc_offer(url, offer, response, error);
            if (!posted || response.status != 201U || !peer->establish(response.body, error))
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
                auto session = std::make_shared<play_session>(results, peer, response.location, index, 102U, started);
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
            results->changed.wait_for(lock,
                                      std::chrono::seconds(15),
                                      [&]() { return results->media_ready.load() + results->establishment_failures.load() == config.viewers; });
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
                      << " p95=" << latencies[(latencies.size() - 1U) * 95U / 100U]
                      << " p99=" << latencies[(latencies.size() - 1U) * 99U / 100U]
                      << " max=" << latencies.back() << '\n';
        }
        std::cout << "phase=established requested=" << config.viewers << " ready=" << results->ready.load()
                  << " media_ready=" << results->media_ready.load()
                  << " establishment_failures=" << results->establishment_failures.load()
                  << " ramp_seconds=" << std::chrono::duration<double>(clock_type::now() - ramp_started).count()
                  << " generator_cpu_seconds=" << established.cpu_seconds << " generator_rss_kib=" << established.rss_kib
                  << " generator_pss_kib=" << established.pss_kib << " generator_fds=" << established.fds << std::endl;
        if (results->media_ready.load() != config.viewers)
        {
            std::size_t missing_reported{};
            for (const auto& session : sessions)
            {
                if ((session->video_packets() == 0U || session->audio_packets() == 0U) && missing_reported++ < 20U)
                {
                    std::cerr << "viewer=" << session->index() << " media_not_ready video_packets=" << session->video_packets()
                              << " audio_packets=" << session->audio_packets() << " received_packets=" << session->received_packets()
                              << " received_media_datagrams=" << session->received_media_datagrams()
                              << " unprotect_failures=" << session->unprotect_failures() << '\n';
                }
            }
            for (const auto& error : results->errors)
            {
                std::cerr << error << '\n';
            }
            const auto removed = stop_and_remove_sessions(sessions, results, shards);
            std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.viewers << '\n';
            stop_shards(shards);
            return 1;
        }

        std::this_thread::sleep_for(config.warmup);
        std::cout << "phase=measurement_start" << std::endl;
        const auto before = total_media(sessions);
        struct session_sample
        {
            std::uint64_t bytes{};
            std::uint64_t video{};
            std::uint64_t audio{};
        };
        std::vector<session_sample> session_before;
        for (const auto& session : sessions)
        {
            session_before.push_back({session->received_bytes(), session->video_packets(), session->audio_packets()});
        }
        const auto process_before = sample_process();
        const auto started = clock_type::now();
        std::this_thread::sleep_for(config.duration);
        const auto elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        const auto process_after = sample_process();
        const auto after = total_media(sessions);
        std::size_t progressing{};
        std::vector<std::uint64_t> viewer_bytes;
        viewer_bytes.reserve(sessions.size());
        std::uint64_t min_video_packets = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t min_audio_packets = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            const auto video = sessions[index]->video_packets() - session_before[index].video;
            const auto audio = sessions[index]->audio_packets() - session_before[index].audio;
            viewer_bytes.push_back(sessions[index]->received_bytes() - session_before[index].bytes);
            min_video_packets = std::min(min_video_packets, video);
            min_audio_packets = std::min(min_audio_packets, audio);
            progressing += video > 0 && audio > 0 ? 1U : 0U;
        }
        std::ranges::sort(viewer_bytes);
        const auto viewer_rate = [&](std::size_t percentile)
        { return static_cast<double>(viewer_bytes[(viewer_bytes.size() - 1U) * percentile / 100U]) / elapsed; };
        std::cout << "phase=measurement progressing=" << progressing << " runtime_failures=" << results->runtime_failures.load()
                  << " duration_seconds=" << elapsed << " received_bytes=" << after.bytes - before.bytes
                  << " received_packets=" << after.packets - before.packets << " video_packets=" << after.video - before.video
                  << " audio_packets=" << after.audio - before.audio << " media_datagrams=" << after.media_datagrams - before.media_datagrams
                  << " unprotect_failures=" << after.unprotect_failures
                  << " bytes_per_viewer_second=" << static_cast<double>(after.bytes - before.bytes) / elapsed / static_cast<double>(config.viewers)
                  << " viewer_bytes_per_second_min=" << viewer_rate(0) << " viewer_bytes_per_second_p10=" << viewer_rate(10)
                  << " viewer_bytes_per_second_p50=" << viewer_rate(50) << " viewer_bytes_per_second_p90=" << viewer_rate(90)
                  << " viewer_bytes_per_second_max=" << viewer_rate(100) << " viewer_video_packets_min=" << min_video_packets
                  << " viewer_audio_packets_min=" << min_audio_packets
                  << " generator_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " generator_rss_kib=" << process_after.rss_kib << " generator_pss_kib=" << process_after.pss_kib
                  << " generator_fds=" << process_after.fds << std::endl;

        const auto removed = stop_and_remove_sessions(sessions, results, shards);
        std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.viewers << '\n';
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
        stop_shards(shards);
        return progressing == config.viewers && results->runtime_failures.load() == 0U && results->stopped.load() == config.viewers &&
                       removed == config.viewers
                   ? 0
                   : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fanout WHEP player failed: " << error.what() << '\n';
        return 2;
    }
}
