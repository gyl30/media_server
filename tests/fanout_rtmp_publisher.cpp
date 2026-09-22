#include <algorithm>
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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "tests/clients/rtmp_test_client.h"

extern "C"
{
#include "amf0.h"
#include "flv-proto.h"
}

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
    std::string stream_prefix;
    std::string media_host{"127.0.0.1"};
    std::uint16_t media_port{1935};
    std::size_t sources{};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::uint32_t frames_per_second{25};
    std::uint32_t gop{25};
    std::uint64_t bitrate{710'000};
    std::chrono::seconds duration{15};
};

struct media_fixture
{
    std::vector<std::uint8_t> metadata;
    std::vector<std::uint8_t> video_config;
    std::shared_ptr<const std::vector<std::uint8_t>> key_frame;
    std::shared_ptr<const std::vector<std::uint8_t>> delta_frame;
    std::uint64_t bitrate{};
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
    std::atomic_size_t connected{};
    std::atomic_size_t ready{};
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

    void notify() { changed.notify_all(); }
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
        else if (argument == "--stream-prefix")
        {
            config.stream_prefix = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-host")
        {
            config.media_host = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-port")
        {
            config.media_port = static_cast<std::uint16_t>(std::stoul(argument_value(index, argc, argv, argument)));
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
        else if (argument == "--fps")
        {
            config.frames_per_second = static_cast<std::uint32_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--gop")
        {
            config.gop = static_cast<std::uint32_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--bitrate")
        {
            config.bitrate = std::stoull(argument_value(index, argc, argv, argument));
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
    if (config.signaling.host.empty() || config.stream_prefix.find('/') == std::string::npos || config.sources == 0U ||
        config.ramp_per_second == 0U || config.io_threads == 0U || config.io_threads > 64U || config.frames_per_second == 0U || config.gop == 0U ||
        config.duration.count() <= 0)
    {
        throw std::runtime_error(
            "signaling-url, stream-prefix with app, sources, ramp-per-second, io-threads, fps, gop and positive duration are required");
    }
    if (config.bitrate / 8U / config.frames_per_second < 64U)
    {
        throw std::runtime_error("bitrate is too small for the media fixture");
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

std::vector<std::uint8_t> make_video_frame(bool key_frame, std::size_t bytes)
{
    const std::array<std::uint8_t, 10> primary = key_frame ? std::array<std::uint8_t, 10>{0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0}
                                                           : std::array<std::uint8_t, 10>{0x41, 0x9a, 0x20, 0x26, 0x94, 0x10, 0x08, 0x04, 0x02, 0x01};
    std::vector<std::uint8_t> packet;
    packet.reserve(bytes);
    packet.insert(packet.end(), {static_cast<std::uint8_t>(key_frame ? 0x17U : 0x27U), 0x01, 0x00, 0x00, 0x00});
    const auto append_nalu = [&packet](std::span<const std::uint8_t> nalu)
    {
        const auto size = static_cast<std::uint32_t>(nalu.size());
        packet.insert(packet.end(),
                      {static_cast<std::uint8_t>(size >> 24U),
                       static_cast<std::uint8_t>(size >> 16U),
                       static_cast<std::uint8_t>(size >> 8U),
                       static_cast<std::uint8_t>(size)});
        packet.insert(packet.end(), nalu.begin(), nalu.end());
    };
    append_nalu(primary);
    if (packet.size() + 5U < bytes)
    {
        std::vector<std::uint8_t> filler(bytes - packet.size() - 4U, 0xffU);
        filler.front() = 0x0c;
        filler.back() = 0x80;
        append_nalu(filler);
    }
    return packet;
}

media_fixture make_media_fixture(const configuration& config)
{
    constexpr std::array<std::uint8_t, 21> sps{0x67, 0x42, 0xc0, 0x0a, 0xda, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03,
                                               0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x28, 0xf1, 0x22, 0x6a};
    constexpr std::array<std::uint8_t, 4> pps{0x68, 0xce, 0x0f, 0xc8};
    media_fixture fixture;
    std::array<std::uint8_t, 256> metadata{};
    auto* end = AMFWriteString(metadata.data(), metadata.data() + metadata.size(), "onMetaData", 10);
    end = AMFWriteECMAArarry(end, metadata.data() + metadata.size());
    end = AMFWriteNamedDouble(end, metadata.data() + metadata.size(), "videocodecid", 12, FLV_VIDEO_H264);
    end = AMFWriteObjectEnd(end, metadata.data() + metadata.size());
    if (end == nullptr)
    {
        throw std::runtime_error("failed to build RTMP metadata");
    }
    fixture.metadata.assign(metadata.data(), end);
    fixture.video_config = {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, sps[1], sps[2], sps[3], 0xff, 0xe1, 0x00, static_cast<std::uint8_t>(sps.size())};
    fixture.video_config.insert(fixture.video_config.end(), sps.begin(), sps.end());
    fixture.video_config.insert(fixture.video_config.end(), {0x01, 0x00, static_cast<std::uint8_t>(pps.size())});
    fixture.video_config.insert(fixture.video_config.end(), pps.begin(), pps.end());
    const auto bytes_per_frame = static_cast<std::size_t>(config.bitrate / 8U / config.frames_per_second);
    fixture.key_frame = std::make_shared<const std::vector<std::uint8_t>>(make_video_frame(true, bytes_per_frame));
    fixture.delta_frame = std::make_shared<const std::vector<std::uint8_t>>(make_video_frame(false, bytes_per_frame));
    fixture.bitrate = static_cast<std::uint64_t>(fixture.delta_frame->size()) * config.frames_per_second * 8U;
    return fixture;
}

class allocation_client final
{
   public:
    explicit allocation_client(endpoint target) : target_(std::move(target)), resolver_(io_), stream_(io_) {}

    std::string allocate(std::string_view stream_name)
    {
        namespace http = boost::beast::http;
        if (!stream_.socket().is_open())
        {
            boost::system::error_code error;
            const auto endpoints = resolver_.resolve(target_.host, target_.port, error);
            if (error)
            {
                throw std::runtime_error("allocation resolve: " + error.message());
            }
            stream_.expires_after(std::chrono::seconds{5});
            stream_.connect(endpoints, error);
            if (error)
            {
                throw std::runtime_error("allocation connect: " + error.message());
            }
        }
        boost::json::object body;
        body["protocol"] = "rtmp";
        body["stream_name"] = stream_name;
        http::request<http::string_body> request{http::verb::post, "/api/publish/allocations", 11};
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
            throw std::runtime_error("allocation write: " + error.message());
        }
        http::response<http::string_body> response;
        http::read(stream_, buffer_, response, error);
        if (error)
        {
            throw std::runtime_error("allocation read: " + error.message());
        }
        if (response.result() != http::status::created)
        {
            throw std::runtime_error("allocation HTTP status " + std::to_string(response.result_int()) + ": " + response.body());
        }
        const auto object = boost::json::parse(response.body()).as_object();
        const auto stream_id = object.at("stream_id").as_string();
        if (!response.keep_alive())
        {
            stream_.socket().close(error);
            buffer_.consume(buffer_.size());
        }
        return std::string(stream_id);
    }

   private:
    endpoint target_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
};

class publish_session final : public std::enable_shared_from_this<publish_session>
{
   public:
    publish_session(boost::asio::io_context& io,
                    const configuration& config,
                    std::shared_ptr<const media_fixture> fixture,
                    std::shared_ptr<shared_results> results,
                    std::size_t index,
                    std::string stream_id)
        : config_(config),
          fixture_(std::move(fixture)),
          results_(std::move(results)),
          index_(index),
          pacing_(io),
          client_(io,
                  config_.stream_prefix.substr(0, config_.stream_prefix.find('/')),
                  config_.stream_prefix.substr(config_.stream_prefix.find('/') + 1U) + '/' + std::to_string(index_) +
                      "?stream_id=" + std::move(stream_id))
    {
    }

    void start()
    {
        boost::asio::co_spawn(pacing_.get_executor(), [self = shared_from_this()]() { return self->run(); }, boost::asio::detached);
    }

    void stop()
    {
        intentional_stop_ = true;
        client_.close();
        pacing_.cancel();
    }

    [[nodiscard]] std::uint64_t sent_bytes() const noexcept { return sent_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t sent_frames() const noexcept { return sent_frames_.load(std::memory_order_relaxed); }

   private:
    boost::asio::awaitable<void> run()
    {
        const auto error = co_await client_.publish(config_.media_host, config_.media_port, fixture_->metadata, fixture_->video_config);
        if (error)
        {
            fail("connect_or_publish", error);
            co_return;
        }
        ++results_->connected;
        ++results_->ready;
        results_->notify();
        auto next_frame = clock_type::now();
        std::uint64_t frame_index{};
        while (!intentional_stop_)
        {
            const auto& frame = frame_index % config_.gop == 0U ? *fixture_->key_frame : *fixture_->delta_frame;
            const auto timestamp = static_cast<std::uint32_t>(frame_index * 1'000U / config_.frames_per_second);
            const auto write_error = co_await client_.push_video(frame, timestamp);
            if (write_error)
            {
                if (!intentional_stop_)
                {
                    fail("media_write", write_error);
                }
                co_return;
            }
            sent_bytes_.fetch_add(frame.size(), std::memory_order_relaxed);
            sent_frames_.fetch_add(1U, std::memory_order_relaxed);
            ++frame_index;
            const auto interval = std::chrono::nanoseconds{1'000'000'000LL / config_.frames_per_second};
            next_frame += interval;
            const auto now = clock_type::now();
            if (next_frame <= now)
            {
                const auto missed = static_cast<std::uint64_t>((now - next_frame) / interval + 1U);
                next_frame += interval * missed;
                frame_index += missed;
            }
            pacing_.expires_at(next_frame);
            boost::system::error_code wait_error;
            co_await pacing_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
            if (wait_error)
            {
                break;
            }
        }
        finish();
    }

    void fail(std::string_view phase, const boost::system::error_code& error)
    {
        if (terminal_)
        {
            return;
        }
        if (results_->ready.load() > 0U && sent_frames_.load() > 0U)
        {
            ++results_->runtime_failures;
        }
        else
        {
            ++results_->establishment_failures;
        }
        results_->add_error("source=" + std::to_string(index_) + " phase=" + std::string(phase) + " error=" + std::to_string(error.value()) + ':' +
                            error.message());
        finish();
    }

    void finish()
    {
        if (terminal_)
        {
            return;
        }
        terminal_ = true;
        client_.close();
        ++results_->stopped;
        results_->notify();
    }

   private:
    const configuration& config_;
    std::shared_ptr<const media_fixture> fixture_;
    std::shared_ptr<shared_results> results_;
    std::size_t index_{};
    boost::asio::steady_timer pacing_;
    media_server::test::rtmp_test_client client_;
    std::atomic_uint64_t sent_bytes_{};
    std::atomic_uint64_t sent_frames_{};
    bool intentional_stop_{};
    bool terminal_{};
};

struct io_shard
{
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::thread thread;
};

std::pair<std::uint64_t, std::uint64_t> media_totals(const std::vector<std::shared_ptr<publish_session>>& sessions)
{
    std::uint64_t bytes{};
    std::uint64_t frames{};
    for (const auto& session : sessions)
    {
        bytes += session->sent_bytes();
        frames += session->sent_frames();
    }
    return {bytes, frames};
}

void stop_shards(std::vector<std::unique_ptr<io_shard>>& shards)
{
    for (auto& shard : shards)
    {
        shard->work.reset();
        shard->io.stop();
    }
    for (auto& shard : shards)
    {
        if (shard->thread.joinable())
        {
            shard->thread.join();
        }
    }
}

}    // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto config = parse_arguments(argc, argv);
        auto fixture = std::make_shared<const media_fixture>(make_media_fixture(config));
        auto results = std::make_shared<shared_results>();
        allocation_client allocations(config.signaling);
        std::vector<std::unique_ptr<io_shard>> shards;
        for (std::size_t index = 0; index < config.io_threads; ++index)
        {
            auto shard = std::make_unique<io_shard>();
            shard->thread = std::thread([pointer = shard.get()]() { pointer->io.run(); });
            shards.emplace_back(std::move(shard));
        }

        std::vector<std::shared_ptr<publish_session>> sessions;
        sessions.reserve(config.sources);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.sources; ++index)
        {
            const auto stream_name = config.stream_prefix + '/' + std::to_string(index);
            auto stream_id = allocations.allocate(stream_name);
            ++results->allocated;
            auto& shard = *shards[index % shards.size()];
            auto session = std::make_shared<publish_session>(shard.io, config, fixture, results, index, std::move(stream_id));
            sessions.push_back(session);
            boost::asio::post(shard.io, [session]() { session->start(); });
            const auto next = ramp_started + ramp_interval * static_cast<std::int64_t>(index + 1U);
            if (next > clock_type::now())
            {
                std::this_thread::sleep_until(next);
            }
        }

        {
            std::unique_lock lock(results->mutex);
            results->changed.wait_for(
                lock, std::chrono::seconds{60}, [&]() { return results->ready.load() + results->establishment_failures.load() == config.sources; });
        }
        const auto established = sample_process();
        std::cout << "phase=established requested=" << config.sources << " allocated=" << results->allocated.load()
                  << " connected=" << results->connected.load() << " ready=" << results->ready.load()
                  << " establishment_failures=" << results->establishment_failures.load()
                  << " ramp_seconds=" << std::chrono::duration<double>(clock_type::now() - ramp_started).count()
                  << " generator_cpu_seconds=" << established.cpu_seconds << " generator_rss_kib=" << established.rss_kib
                  << " generator_pss_kib=" << established.pss_kib << " generator_fds=" << established.fds << '\n';
        std::cout << "io_threads=" << config.io_threads << " fps=" << config.frames_per_second << " gop=" << config.gop
                  << " requested_bitrate=" << config.bitrate << " actual_fixture_bitrate=" << fixture->bitrate << '\n';

        if (results->ready.load() != config.sources)
        {
            for (const auto& error : results->errors)
            {
                std::cerr << error << '\n';
            }
            for (std::size_t index = 0; index < sessions.size(); ++index)
            {
                const auto session = sessions[index];
                boost::asio::post(shards[index % shards.size()]->io, [session]() { session->stop(); });
            }
            stop_shards(shards);
            return 1;
        }

        const auto [bytes_before, frames_before] = media_totals(sessions);
        std::vector<std::uint64_t> session_frames_before;
        session_frames_before.reserve(sessions.size());
        for (const auto& session : sessions)
        {
            session_frames_before.push_back(session->sent_frames());
        }
        const auto process_before = sample_process();
        const auto started = clock_type::now();
        std::this_thread::sleep_for(config.duration);
        const auto elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        const auto process_after = sample_process();
        const auto [bytes_after, frames_after] = media_totals(sessions);
        std::size_t progressing{};
        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            progressing += sessions[index]->sent_frames() > session_frames_before[index] ? 1U : 0U;
        }
        std::cout << "phase=measurement progressing=" << progressing << " runtime_failures=" << results->runtime_failures.load()
                  << " duration_seconds=" << elapsed << " sent_bytes=" << bytes_after - bytes_before
                  << " sent_frames=" << frames_after - frames_before
                  << " bytes_per_source_second=" << static_cast<double>(bytes_after - bytes_before) / elapsed / static_cast<double>(config.sources)
                  << " generator_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " generator_rss_kib=" << process_after.rss_kib << " generator_pss_kib=" << process_after.pss_kib
                  << " generator_fds=" << process_after.fds << '\n';

        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            const auto session = sessions[index];
            boost::asio::post(shards[index % shards.size()]->io, [session]() { session->stop(); });
        }
        {
            std::unique_lock lock(results->mutex);
            results->changed.wait_for(lock, std::chrono::seconds{5}, [&]() { return results->stopped.load() == config.sources; });
        }
        std::cout << "phase=disconnect stopped=" << results->stopped.load() << " requested=" << config.sources << '\n';
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
        stop_shards(shards);
        return progressing == config.sources && results->runtime_failures.load() == 0U && results->stopped.load() == config.sources ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fanout RTMP publisher failed: " << error.what() << '\n';
        return 2;
    }
}
