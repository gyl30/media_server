#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
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
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "bench/credential_file.h"
#include "bench/clients/webrtc_client.h"

extern "C"
{
#include "mpeg4-avc.h"
#include <libavformat/avformat.h>
}

namespace
{

using clock_type = std::chrono::steady_clock;

struct configuration
{
    std::string urls_file;
    std::string fixture;
    std::string bind_address{"0.0.0.0"};
    std::vector<std::string> urls;
    std::size_t sources{};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::chrono::seconds duration{15};
};

struct rtp_payload
{
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    bool marker{};
};

struct fixture_frame
{
    std::chrono::nanoseconds at{};
    std::uint32_t timestamp{};
    bool video{};
    std::vector<rtp_payload> packets;
};

struct media_fixture
{
    std::vector<fixture_frame> frames;
    std::chrono::nanoseconds duration{};
};

volatile std::sig_atomic_t stop_requested{};

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

configuration parse_arguments(int argc, char** argv)
{
    configuration config;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--whip-urls-file")
        {
            config.urls_file = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--fixture")
        {
            config.fixture = argument_value(index, argc, argv, argument);
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
        else if (argument == "--bind-address")
        {
            config.bind_address = argument_value(index, argc, argv, argument);
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
    if (config.urls_file.empty() || config.fixture.empty() || config.sources == 0U || config.ramp_per_second == 0U || config.io_threads == 0U ||
        config.io_threads > 64U || config.duration.count() <= 0)
    {
        throw std::runtime_error("fixture, whip-urls-file, positive sources, ramp, io-threads and duration required");
    }
    config.urls = media_server::bench::read_credentials(config.urls_file, config.sources);
    (void)boost::asio::ip::make_address(config.bind_address);
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
    AVFormatContext* input{};
    if (avformat_open_input(&input, config.fixture.c_str(), nullptr, nullptr) < 0)
    {
        throw std::runtime_error("cannot open fixture");
    }
    const auto close_input = [](AVFormatContext* pointer) { avformat_close_input(&pointer); };
    std::unique_ptr<AVFormatContext, decltype(close_input)> owner(input, close_input);
    if (avformat_find_stream_info(input, nullptr) < 0)
    {
        throw std::runtime_error("cannot read fixture tracks");
    }
    const auto video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const auto audio = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (video < 0 || audio < 0 || input->streams[video]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        input->streams[audio]->codecpar->codec_id != AV_CODEC_ID_OPUS)
    {
        throw std::runtime_error("WHIP fixture requires H.264 and Opus (derive audio from the standard fixture)");
    }
    const auto* parameters = input->streams[video]->codecpar;
    mpeg4_avc_t avc{};
    if (mpeg4_avc_decoder_configuration_record_load(parameters->extradata, static_cast<std::size_t>(parameters->extradata_size), &avc) <= 0)
    {
        throw std::runtime_error("fixture requires AVC configuration");
    }
    const auto free_packet = [](AVPacket* pointer) { av_packet_free(&pointer); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    media_fixture fixture;
    int result{};
    while ((result = av_read_frame(input, packet.get())) >= 0)
    {
        if (packet->stream_index == video || packet->stream_index == audio)
        {
            if (packet->pts == AV_NOPTS_VALUE || packet->dts != packet->pts)
            {
                throw std::runtime_error("fixture requires presentation timestamps without B frames");
            }
            const auto* stream = input->streams[packet->stream_index];
            fixture_frame frame;
            frame.video = packet->stream_index == video;
            frame.at = std::chrono::nanoseconds{av_rescale_q(packet->pts, stream->time_base, {1, 1'000'000'000})};
            frame.timestamp = static_cast<std::uint32_t>(av_rescale_q(packet->pts, stream->time_base, {1, frame.video ? 90'000 : 48'000}));
            if (frame.video)
            {
                std::vector<std::uint8_t> annex_b(static_cast<std::size_t>(packet->size) + 8192U);
                const auto bytes = h264_mp4toannexb(&avc, packet->data, static_cast<std::size_t>(packet->size), annex_b.data(), annex_b.size());
                if (bytes <= 0 ||
                    mpeg4_h264_annexb_nalu(
                        annex_b.data(),
                        static_cast<std::size_t>(bytes),
                        [](void* param, const std::uint8_t* nalu, std::size_t size)
                        { append_h264_nalu(*static_cast<std::vector<rtp_payload>*>(param), {nalu, size}); },
                        &frame.packets) != 0 ||
                    frame.packets.empty())
                {
                    throw std::runtime_error("invalid fixture H.264 access unit");
                }
                frame.packets.back().marker = true;
            }
            else
            {
                frame.packets.push_back({std::make_shared<const std::vector<std::uint8_t>>(packet->data, packet->data + packet->size), true});
            }
            fixture.duration = std::max(fixture.duration,
                                        frame.at + std::chrono::nanoseconds{av_rescale_q(packet->duration, stream->time_base, {1, 1'000'000'000})});
            fixture.frames.emplace_back(std::move(frame));
        }
        av_packet_unref(packet.get());
    }
    if (result != AVERROR_EOF || fixture.frames.empty())
    {
        throw std::runtime_error("fixture read failed");
    }
    std::stable_sort(fixture.frames.begin(), fixture.frames.end(), [](const auto& left, const auto& right) { return left.at < right.at; });
    const auto origin = fixture.frames.front().at;
    for (auto& frame : fixture.frames)
    {
        frame.at -= origin;
        const auto rate = frame.video ? 90'000 : 48'000;
        frame.timestamp = static_cast<std::uint32_t>(av_rescale_q(frame.at.count(), {1, 1'000'000'000}, {1, rate}));
    }
    fixture.duration -= origin;
    if (fixture.duration <= fixture.frames.back().at)
    {
        throw std::runtime_error("fixture duration does not include its final packet");
    }
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

class publish_session final : public std::enable_shared_from_this<publish_session>
{
   public:
    publish_session(boost::asio::io_context& io,
                    const configuration& config,
                    std::shared_ptr<const media_fixture> fixture,
                    std::shared_ptr<shared_results> results,
                    std::shared_ptr<media_server::bench::webrtc_client_peer> peer,
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
        origin_ = clock_type::now();
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
        const auto next = origin_ + fixture_->frames[frame_index_].at;
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
        const auto& frame = fixture_->frames[frame_index_];
        const auto rate = frame.video ? 90'000 : 48'000;
        const auto offset = static_cast<std::uint32_t>(av_rescale_q(cycles_ * fixture_->duration.count(), {1, 1'000'000'000}, {1, rate}));
        for (const auto& payload : frame.packets)
        {
            if (!send_packet(*payload.bytes,
                             frame.video ? 102 : 111,
                             payload.marker,
                             frame.video ? video_sequence_ : audio_sequence_,
                             frame.timestamp + offset,
                             frame.video ? video_ssrc_ : audio_ssrc_,
                             frame.video ? '0' : '1'))
            {
                return;
            }
        }
        ++frame_index_;
        if (frame_index_ == fixture_->frames.size())
        {
            frame_index_ = 0;
            ++cycles_;
            origin_ += fixture_->duration;
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
    std::shared_ptr<media_server::bench::webrtc_client_peer> peer_;
    std::string resource_url_;
    std::size_t index_{};
    boost::asio::steady_timer pacing_;
    clock_type::time_point origin_;
    std::size_t frame_index_{};
    std::int64_t cycles_{};
    std::uint32_t video_ssrc_{};
    std::uint32_t audio_ssrc_{};

    std::uint16_t video_sequence_{};
    std::uint16_t audio_sequence_{};
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
        if (media_server::bench::delete_webrtc_resource(session->resource_url(), error))
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
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "--whip-urls-file FILE --fixture H264_OPUS_MKV --sources N\n"
                     "--bind-address IP --ramp-per-second N --io-threads N --duration SECONDS\n";
        return 0;
    }
    std::signal(SIGTERM, [](int) { stop_requested = 1; });
    std::signal(SIGINT, [](int) { stop_requested = 1; });
    try
    {
        const auto config = parse_arguments(argc, argv);
        auto fixture = std::make_shared<const media_fixture>(make_media_fixture(config));
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

        std::vector<std::shared_ptr<publish_session>> sessions;
        sessions.reserve(config.sources);
        const auto offer = context->make_offer(media_server::bench::webrtc_client_direction::publish);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        for (std::size_t index = 0; index < config.sources; ++index)
        {
            const auto& publish_url = config.urls[index];
            auto& shard = *shards[index % shards.size()];
            auto peer =
                std::make_shared<media_server::bench::webrtc_client_peer>(shard.io, context, boost::asio::ip::make_address(config.bind_address));
            media_server::bench::webrtc_http_response response;
            std::string error;
            if (!media_server::bench::post_webrtc_offer(publish_url, offer, response, error) || response.status != 201U ||
                !peer->establish(response.body, error))
            {
                ++results->establishment_failures;
                results->add_error("source=" + std::to_string(index) +
                                   " setup=" + (error.empty() ? "HTTP " + std::to_string(response.status) + " " + response.body : error));
                if (!response.location.empty())
                {
                    std::string delete_error;
                    if (!media_server::bench::delete_webrtc_resource(response.location, delete_error))
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
        std::cout << "phase=established requested=" << config.sources << " ready=" << results->ready.load()
                  << " establishment_failures=" << results->establishment_failures.load()
                  << " ramp_seconds=" << std::chrono::duration<double>(clock_type::now() - ramp_started).count()
                  << " generator_cpu_seconds=" << established.cpu_seconds << " generator_rss_kib=" << established.rss_kib
                  << " generator_pss_kib=" << established.pss_kib << " generator_fds=" << established.fds << '\n';
        std::cout << "io_threads=" << config.io_threads << " fixture=" << config.fixture << " fixture_frames=" << fixture->frames.size()
                  << " fixture_seconds=" << std::chrono::duration<double>(fixture->duration).count() << std::endl;
        if (results->ready.load() != config.sources)
        {
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
        while (!stop_requested && clock_type::now() < started + config.duration)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
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
        stop_shards(shards);
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
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
