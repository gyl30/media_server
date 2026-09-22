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

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
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
    std::string stream_prefix;
    std::size_t sources{};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::uint32_t frames_per_second{25};
    std::uint32_t gop{25};
    std::uint64_t bitrate{710'000};
    std::chrono::seconds duration{15};
};

struct rtp_payload
{
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    bool marker{};
};

struct media_fixture
{
    std::vector<rtp_payload> key_frame;
    std::vector<rtp_payload> delta_frame;
    std::shared_ptr<const std::vector<std::uint8_t>> opus;
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
        config.ramp_per_second == 0U || config.io_threads == 0U || config.io_threads > 64U || config.frames_per_second == 0U ||
        config.frames_per_second > 50U || config.gop == 0U || config.duration.count() <= 0)
    {
        throw std::runtime_error(
            "signaling-url, stream-prefix with app, sources, ramp-per-second, io-threads, fps, gop and positive duration are required");
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

void append_h264_nalu(std::vector<rtp_payload>& packets, std::span<const std::uint8_t> nalu)
{
    constexpr std::size_t max_payload = 1150U;
    if (nalu.size() <= max_payload)
    {
        packets.push_back({std::make_shared<const std::vector<std::uint8_t>>(nalu.begin(), nalu.end()), false});
        return;
    }
    const auto indicator = static_cast<std::uint8_t>((nalu.front() & 0xe0U) | 28U);
    const auto type = static_cast<std::uint8_t>(nalu.front() & 0x1fU);
    std::size_t offset = 1U;
    while (offset < nalu.size())
    {
        const auto size = std::min(max_payload - 2U, nalu.size() - offset);
        std::vector<std::uint8_t> payload;
        payload.reserve(size + 2U);
        payload.push_back(indicator);
        payload.push_back(static_cast<std::uint8_t>(type | (offset == 1U ? 0x80U : 0U) | (offset + size == nalu.size() ? 0x40U : 0U)));
        payload.insert(payload.end(), nalu.begin() + static_cast<std::ptrdiff_t>(offset), nalu.begin() + static_cast<std::ptrdiff_t>(offset + size));
        packets.push_back({std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)), false});
        offset += size;
    }
}

media_fixture make_media_fixture(const configuration& config)
{
    constexpr std::array<std::uint8_t, 21> sps{0x67, 0x42, 0xc0, 0x0a, 0xda, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03,
                                               0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x28, 0xf1, 0x22, 0x6a};
    constexpr std::array<std::uint8_t, 4> pps{0x68, 0xce, 0x0f, 0xc8};
    const auto bytes_per_frame = std::max<std::size_t>(64U, config.bitrate / 8U / config.frames_per_second);
    std::vector<std::uint8_t> key(bytes_per_frame, 0xffU);
    std::vector<std::uint8_t> delta(bytes_per_frame, 0xffU);
    const std::array<std::uint8_t, 10> key_prefix{0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};
    const std::array<std::uint8_t, 10> delta_prefix{0x41, 0x9a, 0x20, 0x26, 0x94, 0x10, 0x08, 0x04, 0x02, 0x01};
    std::copy(key_prefix.begin(), key_prefix.end(), key.begin());
    std::copy(delta_prefix.begin(), delta_prefix.end(), delta.begin());
    key.back() = 0x80U;
    delta.back() = 0x80U;

    media_fixture fixture;
    append_h264_nalu(fixture.key_frame, sps);
    append_h264_nalu(fixture.key_frame, pps);
    append_h264_nalu(fixture.key_frame, key);
    append_h264_nalu(fixture.delta_frame, delta);
    fixture.key_frame.back().marker = true;
    fixture.delta_frame.back().marker = true;
    fixture.opus = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{0xf8, 0xff, 0xfe});
    fixture.bitrate = static_cast<std::uint64_t>(bytes_per_frame) * config.frames_per_second * 8U;
    return fixture;
}

std::vector<std::uint8_t> make_rtp_packet(std::span<const std::uint8_t> payload,
                                          std::uint8_t payload_type,
                                          bool marker,
                                          std::uint16_t sequence,
                                          std::uint32_t timestamp,
                                          std::uint32_t ssrc,
                                          char mid)
{
    std::vector<std::uint8_t> packet(20U + payload.size());
    packet[0] = 0x90U;
    packet[1] = static_cast<std::uint8_t>(payload_type | (marker ? 0x80U : 0U));
    packet[2] = static_cast<std::uint8_t>(sequence >> 8U);
    packet[3] = static_cast<std::uint8_t>(sequence);
    packet[4] = static_cast<std::uint8_t>(timestamp >> 24U);
    packet[5] = static_cast<std::uint8_t>(timestamp >> 16U);
    packet[6] = static_cast<std::uint8_t>(timestamp >> 8U);
    packet[7] = static_cast<std::uint8_t>(timestamp);
    packet[8] = static_cast<std::uint8_t>(ssrc >> 24U);
    packet[9] = static_cast<std::uint8_t>(ssrc >> 16U);
    packet[10] = static_cast<std::uint8_t>(ssrc >> 8U);
    packet[11] = static_cast<std::uint8_t>(ssrc);
    packet[12] = 0xbeU;
    packet[13] = 0xdeU;
    packet[14] = 0;
    packet[15] = 1;
    packet[16] = 0x40U;
    packet[17] = static_cast<std::uint8_t>(mid);
    std::copy(payload.begin(), payload.end(), packet.begin() + 20);
    return packet;
}

class allocation_client final
{
   public:
    explicit allocation_client(endpoint target) : target_(std::move(target)), resolver_(io_), stream_(io_) {}

    std::pair<std::string, std::string> allocate(std::string_view stream_name)
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
        body["protocol"] = "whip";
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
        if (error || response.result() != http::status::created)
        {
            throw std::runtime_error(error ? "allocation read: " + error.message()
                                           : "allocation HTTP status " + std::to_string(response.result_int()) + ": " + response.body());
        }
        const auto object = boost::json::parse(response.body()).as_object();
        const auto stream_id = object.at("stream_id").as_string();
        const auto publish_url = object.at("publish_url").as_string();
        if (!response.keep_alive())
        {
            stream_.socket().close(error);
            buffer_.consume(buffer_.size());
        }
        return {std::string(stream_id), std::string(publish_url)};
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
                    std::shared_ptr<media_server::test::webrtc_test_peer> peer,
                    std::string resource_url,
                    std::size_t index)
        : config_(config),
          fixture_(std::move(fixture)),
          results_(std::move(results)),
          peer_(std::move(peer)),
          resource_url_(std::move(resource_url)),
          index_(index),
          pacing_(io),
          video_ssrc_(0x10000000U + static_cast<std::uint32_t>(index)),
          audio_ssrc_(0x20000000U + static_cast<std::uint32_t>(index))
    {
    }

    void start()
    {
        auto self = shared_from_this();
        peer_->start_receive(
            [self](bool rtcp, std::span<const std::uint8_t> packet)
            {
                if (rtcp)
                {
                    ++self->received_rtcp_;
                }
                self->received_bytes_.fetch_add(packet.size(), std::memory_order_relaxed);
            },
            [self](const boost::system::error_code& error)
            {
                if (!self->intentional_stop_)
                {
                    self->fail("udp_receive", error.message());
                }
            });
        next_audio_ = next_video_ = clock_type::now();
        schedule();
    }

    void stop()
    {
        if (terminal_)
        {
            return;
        }
        intentional_stop_ = true;
        terminal_ = true;
        pacing_.cancel();
        peer_->close();
        ++results_->stopped;
        results_->changed.notify_all();
    }

    [[nodiscard]] std::uint64_t sent_bytes() const noexcept { return sent_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t sent_packets() const noexcept { return sent_packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] const std::string& resource_url() const noexcept { return resource_url_; }

   private:
    bool send_packet(std::span<const std::uint8_t> payload,
                     std::uint8_t payload_type,
                     bool marker,
                     std::uint16_t& sequence,
                     std::uint32_t timestamp,
                     std::uint32_t ssrc,
                     char mid)
    {
        const auto packet = make_rtp_packet(payload, payload_type, marker, sequence++, timestamp, ssrc, mid);
        if (!peer_->send_rtp(packet))
        {
            fail("srtp_send", "send failed");
            return false;
        }
        sent_bytes_.fetch_add(packet.size(), std::memory_order_relaxed);
        sent_packets_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    void schedule()
    {
        if (terminal_)
        {
            return;
        }
        const auto next = std::min(next_audio_, next_video_);
        pacing_.expires_at(next);
        pacing_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& error)
            {
                if (error || self->terminal_)
                {
                    return;
                }
                self->send_due_media();
                self->schedule();
            });
    }

    void send_due_media()
    {
        const auto now = clock_type::now();
        const auto audio_interval = std::chrono::milliseconds(20);
        if (next_audio_ <= now)
        {
            if (!send_packet(*fixture_->opus, 111, true, audio_sequence_, audio_timestamp_, audio_ssrc_, '1'))
            {
                return;
            }
            audio_timestamp_ += 960U;
            do
            {
                next_audio_ += audio_interval;
            } while (next_audio_ <= now);
        }

        const auto video_interval = std::chrono::nanoseconds{1'000'000'000LL / config_.frames_per_second};
        if (next_video_ <= now && !terminal_)
        {
            const auto& frame = video_frame_ % config_.gop == 0U ? fixture_->key_frame : fixture_->delta_frame;
            for (const auto& payload : frame)
            {
                if (!send_packet(*payload.bytes, 102, payload.marker, video_sequence_, video_timestamp_, video_ssrc_, '0'))
                {
                    return;
                }
            }
            ++video_frame_;
            video_timestamp_ += 90'000U / config_.frames_per_second;
            do
            {
                next_video_ += video_interval;
            } while (next_video_ <= now);
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
        results_->add_error("source=" + std::to_string(index_) + " phase=" + std::string(phase) + " error=" + std::move(detail));
        pacing_.cancel();
        peer_->close();
        ++results_->stopped;
        results_->changed.notify_all();
    }

   private:
    const configuration& config_;
    std::shared_ptr<const media_fixture> fixture_;
    std::shared_ptr<shared_results> results_;
    std::shared_ptr<media_server::test::webrtc_test_peer> peer_;
    std::string resource_url_;
    std::size_t index_{};
    boost::asio::steady_timer pacing_;
    clock_type::time_point next_video_;
    clock_type::time_point next_audio_;
    std::uint32_t video_ssrc_{};
    std::uint32_t audio_ssrc_{};
    std::uint32_t video_timestamp_{};
    std::uint32_t audio_timestamp_{};
    std::uint16_t video_sequence_{};
    std::uint16_t audio_sequence_{};
    std::uint64_t video_frame_{};
    std::atomic_uint64_t sent_bytes_{};
    std::atomic_uint64_t sent_packets_{};
    std::atomic_uint64_t received_bytes_{};
    std::atomic_uint64_t received_rtcp_{};
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

std::pair<std::uint64_t, std::uint64_t> media_totals(const std::vector<std::shared_ptr<publish_session>>& sessions)
{
    std::uint64_t bytes{};
    std::uint64_t packets{};
    for (const auto& session : sessions)
    {
        bytes += session->sent_bytes();
        packets += session->sent_packets();
    }
    return {bytes, packets};
}

std::size_t stop_and_remove_sessions(const std::vector<std::shared_ptr<publish_session>>& sessions,
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
            results->add_error("WHIP DELETE: " + error);
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
        auto fixture = std::make_shared<const media_fixture>(make_media_fixture(config));
        auto context = media_server::test::webrtc_test_context::create();
        if (!context)
        {
            throw std::runtime_error("failed to create WebRTC client context");
        }
        auto results = std::make_shared<shared_results>();
        allocation_client allocations(config.signaling);
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

        std::vector<std::shared_ptr<publish_session>> sessions;
        sessions.reserve(config.sources);
        const auto offer = context->make_offer(media_server::test::webrtc_test_direction::publish);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.sources; ++index)
        {
            const auto stream_name = config.stream_prefix + '/' + std::to_string(index);
            const auto [stream_id, publish_url] = allocations.allocate(stream_name);
            ++results->allocated;
            auto& shard = *shards[index % shards.size()];
            auto peer = std::make_shared<media_server::test::webrtc_test_peer>(shard.io, context);
            media_server::test::webrtc_http_response response;
            std::string error;
            if (!media_server::test::post_webrtc_offer(publish_url, offer, {}, response, error) || response.status != 201U ||
                !peer->establish(response.body, error))
            {
                ++results->establishment_failures;
                results->add_error("source=" + std::to_string(index) +
                                   " setup=" + (error.empty() ? "HTTP " + std::to_string(response.status) + " " + response.body : error));
                if (!response.location.empty())
                {
                    std::string delete_error;
                    if (!media_server::test::delete_webrtc_resource(response.location, delete_error))
                    {
                        results->add_error("WHIP DELETE after setup failure: " + delete_error);
                    }
                }
            }
            else
            {
                auto session = std::make_shared<publish_session>(shard.io, config, fixture, results, peer, response.location, index);
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

        const auto established = sample_process();
        std::cout << "phase=established requested=" << config.sources << " allocated=" << results->allocated.load()
                  << " ready=" << results->ready.load() << " establishment_failures=" << results->establishment_failures.load()
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
            const auto removed = stop_and_remove_sessions(sessions, results, shards);
            std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.sources << '\n';
            stop_shards(shards);
            return 1;
        }

        const auto [bytes_before, packets_before] = media_totals(sessions);
        std::vector<std::uint64_t> session_packets_before;
        for (const auto& session : sessions)
        {
            session_packets_before.push_back(session->sent_packets());
        }
        const auto process_before = sample_process();
        const auto started = clock_type::now();
        std::this_thread::sleep_for(config.duration);
        const auto elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        const auto process_after = sample_process();
        const auto [bytes_after, packets_after] = media_totals(sessions);
        std::size_t progressing{};
        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            progressing += sessions[index]->sent_packets() > session_packets_before[index] ? 1U : 0U;
        }
        std::cout << "phase=measurement progressing=" << progressing << " runtime_failures=" << results->runtime_failures.load()
                  << " duration_seconds=" << elapsed << " sent_bytes=" << bytes_after - bytes_before
                  << " sent_packets=" << packets_after - packets_before
                  << " bytes_per_source_second=" << static_cast<double>(bytes_after - bytes_before) / elapsed / static_cast<double>(config.sources)
                  << " generator_cpu_cores=" << (process_after.cpu_seconds - process_before.cpu_seconds) / elapsed
                  << " generator_rss_kib=" << process_after.rss_kib << " generator_pss_kib=" << process_after.pss_kib
                  << " generator_fds=" << process_after.fds << '\n';

        const auto removed = stop_and_remove_sessions(sessions, results, shards);
        std::cout << "phase=disconnect stopped=" << results->stopped.load() << " removed=" << removed << " requested=" << config.sources << '\n';
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
        stop_shards(shards);
        return progressing == config.sources && results->runtime_failures.load() == 0U && results->stopped.load() == config.sources &&
                       removed == config.sources
                   ? 0
                   : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fanout WHIP publisher failed: " << error.what() << '\n';
        return 2;
    }
}
