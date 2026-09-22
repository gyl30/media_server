#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "tests/clients/webrtc_test_client.h"

namespace
{

using clock_type = std::chrono::steady_clock;

struct endpoint
{
    std::string host;
    std::string port;
};

struct configuration
{
    endpoint signaling;
    std::string source_id;
    std::size_t viewers{};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
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
    std::atomic_size_t allocated{};
    std::atomic_size_t ready{};
    std::atomic_size_t media_ready{};
    std::atomic_size_t establishment_failures{};
    std::atomic_size_t runtime_failures{};
    std::atomic_size_t stopped{};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> errors;

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

endpoint parse_http_endpoint(std::string_view value)
{
    constexpr std::string_view prefix = "http://";
    if (!value.starts_with(prefix))
    {
        throw std::runtime_error("signaling URL must use http://");
    }
    value.remove_prefix(prefix.size());
    value = value.substr(0, value.find('/'));
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos)
    {
        return {std::string(value), "80"};
    }
    return {std::string(value.substr(0, colon)), std::string(value.substr(colon + 1U))};
}

configuration parse_arguments(int argc, char** argv)
{
    configuration config;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--signaling-url")
        {
            config.signaling = parse_http_endpoint(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--source-id")
        {
            config.source_id = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--viewers")
        {
            config.viewers = std::stoull(argument_value(index, argc, argv, argument));
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
        else
        {
            throw std::runtime_error("unknown argument " + std::string(argument));
        }
    }
    if (config.signaling.host.empty() || config.source_id.empty() || config.viewers == 0U || config.ramp_per_second == 0U ||
        config.io_threads == 0U || config.io_threads > 64U || config.duration.count() <= 0)
    {
        throw std::runtime_error("signaling-url, source-id, viewers, ramp-per-second, io-threads and positive duration are required");
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

class preview_client final
{
   public:
    explicit preview_client(endpoint target) : target_(std::move(target)), resolver_(io_), stream_(io_) {}

    std::pair<std::string, std::string> allocate(std::string_view source_id)
    {
        namespace http = boost::beast::http;
        if (!stream_.socket().is_open())
        {
            boost::system::error_code error;
            const auto endpoints = resolver_.resolve(target_.host, target_.port, error);
            if (error)
            {
                throw std::runtime_error("preview resolve: " + error.message());
            }
            stream_.expires_after(std::chrono::seconds{5});
            stream_.connect(endpoints, error);
            if (error)
            {
                throw std::runtime_error("preview connect: " + error.message());
            }
        }
        boost::json::object body;
        body["source_id"] = source_id;
        http::request<http::string_body> request{http::verb::post, "/api/preview/start", 11};
        request.set(http::field::host, target_.host);
        request.set(http::field::content_type, "application/json");
        request.keep_alive(true);
        request.body() = boost::json::serialize(body);
        request.prepare_payload();
        boost::system::error_code error;
        stream_.expires_after(std::chrono::seconds{5});
        http::write(stream_, request, error);
        if (error)
        {
            throw std::runtime_error("preview write: " + error.message());
        }
        http::response<http::string_body> response;
        http::read(stream_, buffer_, response, error);
        if (error || response.result() != http::status::created)
        {
            throw std::runtime_error(error ? "preview read: " + error.message()
                                           : "preview HTTP status " + std::to_string(response.result_int()) + ": " + response.body());
        }
        const auto object = boost::json::parse(response.body()).as_object();
        const auto stream_id = object.at("stream_id").as_string();
        const auto whep_url = object.at("whep_url").as_string();
        if (!response.keep_alive())
        {
            stream_.socket().close(error);
            buffer_.consume(buffer_.size());
        }
        return {std::string(stream_id), std::string(whep_url)};
    }

   private:
    endpoint target_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
};

std::uint32_t read_u32(std::span<const std::uint8_t> packet, std::size_t offset)
{
    return (static_cast<std::uint32_t>(packet[offset]) << 24U) | (static_cast<std::uint32_t>(packet[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(packet[offset + 2U]) << 8U) | packet[offset + 3U];
}

class play_session final : public std::enable_shared_from_this<play_session>
{
   public:
    play_session(std::shared_ptr<shared_results> results,
                 std::shared_ptr<media_server::test::webrtc_test_peer> peer,
                 std::string resource_url,
                 std::size_t index)
        : results_(std::move(results)), peer_(std::move(peer)), resource_url_(std::move(resource_url)), index_(index)
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
        if (payload_type == 102U)
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
            ++results_->media_ready;
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
    std::shared_ptr<media_server::test::webrtc_test_peer> peer_;
    std::string resource_url_;
    std::size_t index_{};
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
        if (media_server::test::delete_webrtc_resource(session->resource_url(), error))
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
        auto context = media_server::test::webrtc_test_context::create();
        if (!context)
        {
            throw std::runtime_error("failed to create WebRTC client context");
        }
        auto results = std::make_shared<shared_results>();
        preview_client previews(config.signaling);
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
        const auto offer = context->make_offer(media_server::test::webrtc_test_direction::play);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.viewers; ++index)
        {
            const auto [stream_id, whep_url] = previews.allocate(config.source_id);
            ++results->allocated;
            auto& shard = *shards[index % shards.size()];
            auto peer = std::make_shared<media_server::test::webrtc_test_peer>(shard.io, context);
            media_server::test::webrtc_http_response response;
            std::string error;
            bool posted = false;
            const auto deadline = clock_type::now() + std::chrono::seconds(5);
            do
            {
                posted = media_server::test::post_webrtc_offer(whep_url, offer, stream_id, response, error);
                if (!posted || response.status != 409U)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while (clock_type::now() < deadline);
            if (!posted || response.status != 201U || !peer->establish(response.body, error))
            {
                ++results->establishment_failures;
                results->add_error("viewer=" + std::to_string(index) +
                                   " setup=" + (error.empty() ? "HTTP " + std::to_string(response.status) + " " + response.body : error));
                if (!response.location.empty())
                {
                    std::string delete_error;
                    if (!media_server::test::delete_webrtc_resource(response.location, delete_error))
                    {
                        results->add_error("WHEP DELETE after setup failure: " + delete_error);
                    }
                }
            }
            else
            {
                auto session = std::make_shared<play_session>(results, peer, response.location, index);
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
        std::cout << "phase=established requested=" << config.viewers << " allocated=" << results->allocated.load()
                  << " ready=" << results->ready.load() << " media_ready=" << results->media_ready.load()
                  << " establishment_failures=" << results->establishment_failures.load()
                  << " ramp_seconds=" << std::chrono::duration<double>(clock_type::now() - ramp_started).count()
                  << " generator_cpu_seconds=" << established.cpu_seconds << " generator_rss_kib=" << established.rss_kib
                  << " generator_pss_kib=" << established.pss_kib << " generator_fds=" << established.fds << '\n';
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

        const auto before = total_media(sessions);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> session_before;
        for (const auto& session : sessions)
        {
            session_before.emplace_back(session->video_packets(), session->audio_packets());
        }
        const auto process_before = sample_process();
        const auto started = clock_type::now();
        std::this_thread::sleep_for(config.duration);
        const auto elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        const auto process_after = sample_process();
        const auto after = total_media(sessions);
        std::size_t progressing{};
        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            progressing +=
                sessions[index]->video_packets() > session_before[index].first && sessions[index]->audio_packets() > session_before[index].second
                    ? 1U
                    : 0U;
        }
        std::cout << "phase=measurement progressing=" << progressing << " runtime_failures=" << results->runtime_failures.load()
                  << " duration_seconds=" << elapsed << " received_bytes=" << after.bytes - before.bytes
                  << " received_packets=" << after.packets - before.packets << " video_packets=" << after.video - before.video
                  << " audio_packets=" << after.audio - before.audio << " media_datagrams=" << after.media_datagrams - before.media_datagrams
                  << " unprotect_failures=" << after.unprotect_failures
                  << " bytes_per_viewer_second=" << static_cast<double>(after.bytes - before.bytes) / elapsed / static_cast<double>(config.viewers)
                  << " generator_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " generator_rss_kib=" << process_after.rss_kib << " generator_pss_kib=" << process_after.pss_kib
                  << " generator_fds=" << process_after.fds << '\n';

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
