#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"
#include "media/hls/hls_service.h"
#include "media/net/tcp_connection.h"
#include "media/net/io_context_pool.h"
#include "media/net/tcp_listener.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/http/http_flv_output.h"
#include "media/http/http_session.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/webrtc/webrtc_output.h"
#include "media/webrtc/whep_service.h"

extern "C"
{
#include "amf0.h"
#include "flv-demuxer.h"
#include "flv-header.h"
#include "flv-parser.h"
#include "flv-proto.h"
#include "mpeg-ts.h"
#include "rtmp-chunk-header.h"
#include "rtmp-client.h"
#include "rtmp-internal.h"
#include "rtmp-msgtypeid.h"
#include "rtp-packet.h"
#include "rtp-payload.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "rtsp-muxer.h"
#include "rtsp-payloads.h"
}

#include <algorithm>
#include <atomic>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

const std::vector<std::uint8_t> h264_config{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
    0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
};

const std::vector<std::uint8_t> h264_config_updated{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xba, 0x6a, 0x02, 0x1a, 0x02, 0x80, 0x00,
    0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8c, 0x18, 0xcb, 0x00, 0x00, 0x00, 0x01, 0x68, 0xef, 0xbc, 0xb0,
};

const std::vector<std::uint8_t> h265_config{
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x78, 0x9d, 0xc0, 0x90, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x32, 0x16, 0x59, 0xde, 0x49, 0x1b, 0x6b, 0x80, 0x40,
    0x00, 0x00, 0xfa, 0x00, 0x00, 0x17, 0x70, 0x02, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89,
};

const std::vector<std::uint8_t> h265_config_updated{
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0x15, 0xc0, 0x90, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00,
    0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0, 0x05, 0xa2, 0x00, 0x50, 0x16, 0x20,
    0x57, 0xb9, 0x16, 0x54, 0x40, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0, 0x2c, 0xbc, 0x14, 0xc9,
};

const std::vector<std::uint8_t> aac_asc{0x12, 0x10};
const std::vector<std::uint8_t> aac_asc_updated{0x11, 0x90};

const std::vector<std::vector<std::uint8_t>> valid_aac_adts_frames{
    {0xff, 0xf1, 0x50, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61, 0x76, 0x63, 0x36,
     0x31, 0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00, 0x42, 0x20, 0x08, 0xc1, 0x18, 0x38},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
};

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "[fail] " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

struct demuxed_packet
{
    int codec{};
    std::int64_t pts{};
    std::int64_t dts{};
    int flags{};
    std::vector<std::uint8_t> payload;
};

struct flv_demux_capture
{
    std::vector<demuxed_packet> packets;
};

struct http_flv_write
{
    media_reader_generation generation{};
    bool bootstrap{};
    std::vector<std::uint8_t> data;
};

struct http_flv_capture
{
    std::vector<http_flv_write> writes;
    std::size_t ends{};
};

int capture_flv_packet(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags)
{
    const auto* begin = static_cast<const std::uint8_t*>(data);
    static_cast<flv_demux_capture*>(param)->packets.push_back(demuxed_packet{
        .codec = codec,
        .pts = pts,
        .dts = dts,
        .flags = flags,
        .payload = std::vector<std::uint8_t>(begin, begin + bytes),
    });
    return 0;
}

flv_demux_capture demux_http_flv(const http_flv_capture& output)
{
    std::vector<std::uint8_t> data;
    for (const auto& write : output.writes)
    {
        data.insert(data.end(), write.data.begin(), write.data.end());
    }

    flv_demux_capture capture;
    flv_parser_t parser{};
    require(!data.empty() && flv_parser_input(&parser, data.data(), data.size(), &capture_flv_packet, &capture) == 0,
            "http flv parser input");
    return capture;
}

struct ts_demux_capture
{
    std::vector<int> stream_codecs;
    std::vector<demuxed_packet> packets;
};

int capture_ts_packet(void* param, int, int, int codec, int flags, std::int64_t pts, std::int64_t dts, const void* data, std::size_t bytes)
{
    const auto* begin = static_cast<const std::uint8_t*>(data);
    static_cast<ts_demux_capture*>(param)->packets.push_back(demuxed_packet{
        .codec = codec,
        .pts = pts,
        .dts = dts,
        .flags = flags,
        .payload = std::vector<std::uint8_t>(begin, begin + bytes),
    });
    return 0;
}

void capture_ts_stream(void* param, int, int codec, const void*, int, int) { static_cast<ts_demux_capture*>(param)->stream_codecs.push_back(codec); }

ts_demux_capture demux_ts_segment(std::span<const std::uint8_t> segment)
{
    require(!segment.empty() && segment.size() % 188U == 0U, "mpeg-ts packet alignment");
    ts_demux_capture capture;
    auto* demuxer = ts_demuxer_create(&capture_ts_packet, &capture);
    require(demuxer != nullptr, "mpeg-ts demuxer create");
    ts_demuxer_notify_t notify{.onstream = &capture_ts_stream};
    ts_demuxer_set_notify(demuxer, &notify, &capture);
    for (std::size_t offset = 0; offset < segment.size(); offset += 188U)
    {
        require(ts_demuxer_input(demuxer, segment.data() + offset, 188U) == 0, "mpeg-ts demuxer input");
    }
    require(ts_demuxer_flush(demuxer) == 0, "mpeg-ts demuxer flush");
    require(ts_demuxer_destroy(demuxer) == 0, "mpeg-ts demuxer destroy");
    std::ranges::sort(capture.stream_codecs);
    const auto unique_end = std::ranges::unique(capture.stream_codecs).begin();
    capture.stream_codecs.erase(unique_end, capture.stream_codecs.end());
    return capture;
}

int validate_flv_aac_config(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t, std::uint32_t, int)
{
    if (codec != FLV_AUDIO_ASC || data == nullptr)
    {
        return 0;
    }
    const auto config = parse_aac_asc(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes));
    *static_cast<bool*>(param) = config.has_value();
    return config.has_value() ? 0 : -1;
}

bool accepts_flv_aac_config(std::span<const std::uint8_t> asc)
{
    bool accepted = false;
    const auto demuxer =
        std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(flv_demuxer_create(&validate_flv_aac_config, &accepted), &flv_demuxer_destroy);
    require(demuxer != nullptr, "flv aac config demuxer");

    std::vector<std::uint8_t> sequence_header{0xaf, FLV_SEQUENCE_HEADER};
    sequence_header.insert(sequence_header.end(), asc.begin(), asc.end());
    const auto result = flv_demuxer_input(demuxer.get(), FLV_TYPE_AUDIO, sequence_header.data(), sequence_header.size(), 0);
    return result == 0 && accepted;
}

media_track make_video_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h264,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h264_config,
    };
}

media_track make_h265_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h265,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h265_config,
    };
}

media_track make_audio_track()
{
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::aac,
        .clock_rate = 44'100,
        .channel_count = 2,
        .codec_config = aac_asc,
    };
}

media_track make_video_track(std::uint8_t config_marker)
{
    auto track = make_video_track();
    track.codec_config.push_back(config_marker);
    return track;
}

media_track make_audio_track(std::uint8_t config_marker)
{
    auto track = make_audio_track();
    track.codec_config.push_back(config_marker);
    return track;
}

media_frame make_video_frame(std::int64_t pts_ns, bool key_frame)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes = h264_config;
        const std::vector<std::uint8_t> idr{
            0x00,
            0x00,
            0x00,
            0x01,
            0x65,
            0x88,
            0x84,
            0x21,
            0xa0,
        };
        bytes.insert(bytes.end(), idr.begin(), idr.end());
    }
    else
    {
        bytes = {
            0x00,
            0x00,
            0x00,
            0x01,
            0x41,
            0x9a,
            0x22,
            0x11,
        };
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = key_frame,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)),
    };
}

media_frame make_large_video_frame(std::int64_t pts_ns)
{
    auto frame = make_video_frame(pts_ns, true);
    auto payload = std::make_shared<std::vector<std::uint8_t>>(8U * 1024U * 1024U, 0x55);
    (*payload)[0] = 0;
    (*payload)[1] = 0;
    (*payload)[2] = 0;
    (*payload)[3] = 1;
    (*payload)[4] = 0x65;
    frame.payload = std::move(payload);
    return frame;
}

media_frame make_h265_frame(std::int64_t pts_ns, bool key_frame)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes = h265_config;
        const std::vector<std::uint8_t> idr{
            0x00,
            0x00,
            0x00,
            0x01,
            0x26,
            0x01,
            0x9a,
            0x20,
            0x11,
        };
        bytes.insert(bytes.end(), idr.begin(), idr.end());
    }
    else
    {
        bytes = {
            0x00,
            0x00,
            0x00,
            0x01,
            0x02,
            0x01,
            0x9a,
            0x20,
        };
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = key_frame,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)),
    };
}

media_frame make_audio_frame(std::int64_t pts_ns)
{
    const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
    auto adts = make_adts_frame(aac_asc, raw);
    require(!adts.empty(), "make adts");
    return media_frame{
        .track = audio_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(adts)),
    };
}

class counting_sink final : public media_sink
{
   public:
    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame& frame) override
    {
        ++frames;
        received_frames.emplace_back(frame.track, frame.pts_ns);
    }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};
    std::vector<std::pair<track_id, std::int64_t>> received_frames;
};

class worker_sink final : public media_sink
{
   public:
    explicit worker_sink(std::atomic_int& ended_count, io_context_pool& workers) : ended_count_(ended_count), workers_(workers) {}

    void on_track(const media_track&) override
    {
        std::scoped_lock lock(mutex_);
        thread_ = std::this_thread::get_id();
        ++tracks_;
    }

    void on_frame(const media_frame& frame) override
    {
        std::scoped_lock lock(mutex_);
        thread_ = std::this_thread::get_id();
        frames_.push_back(frame.pts_ns);
        payloads_.push_back(frame.payload.get());
    }

    void on_end() override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            ++ends_;
        }
        if (ended_count_.fetch_add(1) + 1 == 3)
        {
            workers_.release_work();
        }
    }

    [[nodiscard]] std::thread::id thread() const
    {
        std::scoped_lock lock(mutex_);
        return thread_;
    }

    [[nodiscard]] std::size_t tracks() const
    {
        std::scoped_lock lock(mutex_);
        return tracks_;
    }

    [[nodiscard]] std::size_t ends() const
    {
        std::scoped_lock lock(mutex_);
        return ends_;
    }

    [[nodiscard]] std::vector<std::int64_t> frames() const
    {
        std::scoped_lock lock(mutex_);
        return frames_;
    }

    [[nodiscard]] std::vector<const void*> payloads() const
    {
        std::scoped_lock lock(mutex_);
        return payloads_;
    }

   private:
    std::atomic_int& ended_count_;
    io_context_pool& workers_;
    mutable std::mutex mutex_;
    std::thread::id thread_;
    std::size_t tracks_{};
    std::size_t ends_{};
    std::vector<std::int64_t> frames_;
    std::vector<const void*> payloads_;
};

class pull_test_reader final : public media_reader
{
   public:
    explicit pull_test_reader(bool continuous, bool read_on_ready = true)
        : continuous_(continuous), read_on_ready_(read_on_ready)
    {
    }

    void on_track(media_reader_generation generation, const media_track& track) override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            track_versions_.emplace_back(generation, track.config_version);
        }
        condition_.notify_all();
    }

    void on_ready(media_reader_generation generation) override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            generation_ = generation;
            ready_generations_.push_back(generation);
        }
        if (read_on_ready_)
        {
            reader_handle().async_read(generation);
        }
        condition_.notify_all();
    }

    void on_read(media_reader_generation generation, media_frame frame) override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            frames_.emplace_back(generation, frame.pts_ns);
        }
        if (continuous_)
        {
            reader_handle().async_read(generation);
        }
        condition_.notify_all();
    }

    void on_end(media_reader_generation generation) override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            end_generation_ = generation;
            ++ends_;
        }
        condition_.notify_all();
    }

    void request()
    {
        media_reader_generation generation;
        {
            std::scoped_lock lock(mutex_);
            generation = generation_;
        }
        reader_handle().async_read(generation);
    }

    void remove() { reader_handle().remove(); }

    [[nodiscard]] bool wait_for_ready(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [this, count]() { return ready_generations_.size() >= count; });
    }

    [[nodiscard]] bool wait_for_frames(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [this, count]() { return frames_.size() >= count; });
    }

    [[nodiscard]] bool wait_for_ends(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [this, count]() { return ends_ >= count; });
    }

    [[nodiscard]] std::vector<std::pair<media_reader_generation, std::int64_t>> frames() const
    {
        std::scoped_lock lock(mutex_);
        return frames_;
    }

    [[nodiscard]] std::vector<std::pair<media_reader_generation, std::uint64_t>> track_versions() const
    {
        std::scoped_lock lock(mutex_);
        return track_versions_;
    }

    [[nodiscard]] std::vector<media_reader_generation> ready_generations() const
    {
        std::scoped_lock lock(mutex_);
        return ready_generations_;
    }

    [[nodiscard]] std::size_t ends() const
    {
        std::scoped_lock lock(mutex_);
        return ends_;
    }

    [[nodiscard]] media_reader_generation end_generation() const
    {
        std::scoped_lock lock(mutex_);
        return end_generation_;
    }

    [[nodiscard]] std::thread::id thread() const
    {
        std::scoped_lock lock(mutex_);
        return thread_;
    }

   private:
    bool continuous_{};
    bool read_on_ready_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread::id thread_;
    media_reader_generation generation_{};
    media_reader_generation end_generation_{};
    std::size_t ends_{};
    std::vector<std::pair<media_reader_generation, std::uint64_t>> track_versions_;
    std::vector<media_reader_generation> ready_generations_;
    std::vector<std::pair<media_reader_generation, std::int64_t>> frames_;
};

class self_removing_sink final : public media_sink
{
   public:
    explicit self_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.remove_sink(*this);
    }
    void on_end() override {}

    std::size_t frames{};

   private:
    media_stream& stream_;
};

class track_removing_sink final : public media_sink
{
   public:
    explicit track_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(*this);
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

   private:
    media_stream& stream_;
};

class track_other_removing_sink final : public media_sink
{
   public:
    track_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(target_);
    }
    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::size_t tracks{};

   private:
    media_stream& stream_;
    media_sink& target_;
};

class frame_other_removing_sink final : public media_sink
{
   public:
    frame_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.remove_sink(target_);
    }
    void on_end() override {}

    std::size_t frames{};

   private:
    media_stream& stream_;
    media_sink& target_;
};

class track_ending_sink final : public media_sink
{
   public:
    explicit track_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.end();
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

   private:
    media_stream& stream_;
};

class frame_ending_sink final : public media_sink
{
   public:
    explicit frame_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.end();
    }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

   private:
    media_stream& stream_;
};

class track_updating_sink final : public media_sink
{
   public:
    track_updating_sink(media_stream& stream, track_id trigger_id, std::uint64_t trigger_version, media_track replacement)
        : stream_(stream), trigger_id_(trigger_id), trigger_version_(trigger_version), replacement_(std::move(replacement))
    {
    }

    void on_track(const media_track& track) override
    {
        versions.emplace_back(track.id, track.config_version);
        if (!updated && track.id == trigger_id_ && track.config_version == trigger_version_)
        {
            updated = true;
            update_succeeded = stream_.update_track(replacement_);
        }
    }

    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::vector<std::pair<track_id, std::uint64_t>> versions;
    bool updated{};
    bool update_succeeded{};

   private:
    media_stream& stream_;
    track_id trigger_id_{};
    std::uint64_t trigger_version_{};
    media_track replacement_;
};

class track_version_sink final : public media_sink
{
   public:
    void on_track(const media_track& track) override { versions.emplace_back(track.id, track.config_version); }

    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::vector<std::pair<track_id, std::uint64_t>> versions;
};

class generation_replacing_sink final : public media_sink
{
   public:
    generation_replacing_sink(stream_registry& registry, std::shared_ptr<media_stream> replacement)
        : registry_(registry), replacement_(std::move(replacement))
    {
    }

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override {}
    void on_end() override
    {
        old_generation_hidden = !registry_.find(replacement_->name());
        replacement_added = registry_.add(replacement_);
    }

    bool old_generation_hidden{};
    bool replacement_added{};

   private:
    stream_registry& registry_;
    std::shared_ptr<media_stream> replacement_;
};

struct rtp_timeline_capture
{
    std::vector<std::uint32_t> timestamps;
};

struct h265_rtp_capture
{
    std::vector<std::uint8_t> access_unit;
};

int capture_h265_nalu(void* param, const void* packet, int bytes, std::uint32_t, int)
{
    require(packet != nullptr && bytes > 0, "rtsp h265 depacketized nalu");
    auto& access_unit = static_cast<h265_rtp_capture*>(param)->access_unit;
    constexpr std::array<std::uint8_t, 4> start_code{0x00, 0x00, 0x00, 0x01};
    access_unit.insert(access_unit.end(), start_code.begin(), start_code.end());
    const auto* begin = static_cast<const std::uint8_t*>(packet);
    access_unit.insert(access_unit.end(), begin, begin + bytes);
    return 0;
}

struct rtsp_interleaved_packet
{
    std::uint8_t channel{};
    std::vector<std::uint8_t> payload;
};

struct rtsp_aac_capture
{
    rtsp_demuxer_t* demuxer{};
    avpkt2bs_t bitstream{};
    std::vector<std::uint8_t> rtp_payload;
    std::vector<std::uint8_t> frame;
};

int capture_rtsp_aac_frame(void* param, avpacket_t* packet)
{
    auto& capture = *static_cast<rtsp_aac_capture*>(param);
    const auto bytes = avpkt2bs_input(&capture.bitstream, packet);
    if (bytes > 0)
    {
        capture.frame.assign(capture.bitstream.ptr, capture.bitstream.ptr + bytes);
    }
    return bytes < 0 ? bytes : 0;
}

int forward_rtsp_aac_rtp(void* param, int, const void* packet, int bytes, std::uint32_t, int)
{
    auto& capture = *static_cast<rtsp_aac_capture*>(param);
    rtp_packet_t decoded{};
    require(rtp_packet_deserialize(&decoded, packet, bytes) == 0, "rtsp aac rtp packet");
    const auto* begin = static_cast<const std::uint8_t*>(decoded.payload);
    capture.rtp_payload.assign(begin, begin + decoded.payloadlen);
    return rtsp_demuxer_input(capture.demuxer, packet, bytes);
}

struct rtsp_client_capture
{
    std::string request;
    int setup_timeout{-1};
};

int capture_rtsp_request(void* param, const char*, const void* request, std::size_t bytes)
{
    auto& capture = *static_cast<rtsp_client_capture*>(param);
    capture.request.assign(static_cast<const char*>(request), bytes);
    return static_cast<int>(bytes);
}

int capture_rtsp_rtp_port(void*, int media, const char*, unsigned short port[2], char*, int)
{
    port[0] = static_cast<unsigned short>(media * 2);
    port[1] = static_cast<unsigned short>(media * 2 + 1);
    return RTSP_TRANSPORT_RTP_TCP;
}

int capture_rtsp_setup(void* param, int timeout, std::int64_t)
{
    static_cast<rtsp_client_capture*>(param)->setup_timeout = timeout;
    return 0;
}

int rtsp_setup_timeout(std::string_view session_header)
{
    rtsp_client_capture capture;
    rtsp_client_handler_t handler{};
    handler.send = &capture_rtsp_request;
    handler.rtpport = &capture_rtsp_rtp_port;
    handler.onsetup = &capture_rtsp_setup;

    auto* client = rtsp_client_create("rtsp://127.0.0.1/live/test", nullptr, nullptr, &handler, &capture);
    require(client != nullptr, "rtsp client create");

    constexpr std::string_view sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";
    require(rtsp_client_setup(client, sdp.data(), static_cast<int>(sdp.size())) == 0, "rtsp client setup request");

    const auto response = std::string("RTSP/1.0 200 OK\r\n") + "CSeq: 1\r\n" + "Session: " + std::string(session_header) + "\r\n" +
                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" + "Content-Length: 0\r\n\r\n";
    require(rtsp_client_input(client, response.data(), response.size()) == 0, "rtsp setup response");

    require(rtsp_client_options(client, nullptr) == 0, "rtsp keepalive options");
    require(capture.request.starts_with("OPTIONS * RTSP/1.0\r\n"), "rtsp keepalive method");
    const auto separator = session_header.find(';');
    const auto session_id = session_header.substr(0, separator);
    require(capture.request.find("Session: " + std::string(session_id) + "\r\n") != std::string::npos, "rtsp keepalive session");

    const auto timeout = capture.setup_timeout;
    rtsp_client_destroy(client);
    return timeout;
}

void test_rtsp_client_session_timeout()
{
    require(rtsp_setup_timeout("session-1;timeout=70") == 70, "rtsp setup explicit timeout");
    require(rtsp_setup_timeout("session-2") == 60, "rtsp setup default timeout");
}

int ignore_rtsp_media(void*, int, const char*, unsigned short[2], char*, int) { return 0; }

void test_rtsp_client_rejects_empty_media_selection()
{
    rtsp_client_capture capture;
    rtsp_client_handler_t handler{};
    handler.send = &capture_rtsp_request;
    handler.rtpport = &ignore_rtsp_media;

    auto* client = rtsp_client_create("rtsp://127.0.0.1/live/test", nullptr, nullptr, &handler, &capture);
    require(client != nullptr, "rtsp empty selection client create");

    constexpr std::string_view sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H265/90000\r\n"
        "a=control:trackID=0\r\n";
    require(rtsp_client_setup(client, sdp.data(), static_cast<int>(sdp.size())) != 0, "rtsp reject all ignored media");
    require(capture.request.empty(), "rtsp ignored media sends no setup");
    rtsp_client_destroy(client);
}

std::size_t rtsp_content_length(std::string_view response)
{
    constexpr std::string_view name = "Content-Length:";
    const auto offset = response.find(name);
    if (offset == std::string_view::npos)
    {
        return 0;
    }
    auto begin = offset + name.size();
    while (begin < response.size() && response[begin] == ' ')
    {
        ++begin;
    }
    const auto end = response.find("\r\n", begin);
    require(end != std::string_view::npos, "rtsp response content length end");
    std::size_t length = 0;
    const auto [pointer, error] = std::from_chars(response.data() + begin, response.data() + end, length);
    require(error == std::errc{} && pointer == response.data() + end, "rtsp response content length");
    return length;
}

std::string rtsp_header_value(std::string_view response, std::string_view name)
{
    const auto offset = response.find(name);
    if (offset == std::string_view::npos)
    {
        return {};
    }
    auto begin = offset + name.size();
    while (begin < response.size() && response[begin] == ' ')
    {
        ++begin;
    }
    const auto end = response.find("\r\n", begin);
    require(end != std::string_view::npos, "rtsp response header end");
    return std::string(response.substr(begin, end - begin));
}

std::string read_rtsp_headers(boost::asio::ip::tcp::socket& socket)
{
    std::string request;
    boost::asio::read_until(socket, boost::asio::dynamic_buffer(request), "\r\n\r\n");
    return request;
}

std::string read_rtsp_headers_until(boost::asio::ip::tcp::socket& socket, std::chrono::steady_clock::duration timeout)
{
    socket.non_blocking(true);
    std::string request;
    std::array<char, 2'048> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (request.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
    {
        boost::system::error_code error;
        const auto bytes = socket.read_some(boost::asio::buffer(buffer), error);
        if (!error)
        {
            request.append(buffer.data(), bytes);
        }
        else if (error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        {
            fail("rtsp timed read");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    socket.non_blocking(false);
    return request;
}

std::vector<std::uint8_t> make_rtmp_video_sequence_header(media_track track)
{
    std::vector<std::uint8_t> packet;
    flv_output_muxer muxer(
        [&packet](int type, std::span<const std::uint8_t> data, std::uint32_t)
        {
            if (type == FLV_TYPE_VIDEO && packet.empty())
            {
                packet.assign(data.begin(), data.end());
            }
        });
    muxer.on_track(track);
    require(!packet.empty(), "rtmp video sequence header");
    return packet;
}

struct rtmp_status
{
    std::string level;
    std::string code;
};

rtmp_status parse_rtmp_status(std::span<const std::uint8_t> payload)
{
    std::array<char, 16> command{};
    std::array<char, 16> level{};
    std::array<char, 64> code{};
    double transaction{};
    std::array<amf_object_item_t, 2> information{
        amf_object_item_t{AMF_STRING, "level", level.data(), level.size()},
        amf_object_item_t{AMF_STRING, "code", code.data(), code.size()},
    };
    std::array<amf_object_item_t, 4> items{
        amf_object_item_t{AMF_STRING, "command", command.data(), command.size()},
        amf_object_item_t{AMF_NUMBER, "transaction", &transaction, sizeof(transaction)},
        amf_object_item_t{AMF_OBJECT, "command_object", nullptr, 0},
        amf_object_item_t{AMF_OBJECT, "information", information.data(), information.size()},
    };

    const auto* end = payload.data() + payload.size();
    require(amf_read_items(payload.data(), end, items.data(), items.size()) == end, "rtmp status amf");
    require(std::string_view(command.data()) == "onStatus", "rtmp status command");
    return rtmp_status{.level = level.data(), .code = code.data()};
}

class rtmp_input_test_peer final
{
   public:
    explicit rtmp_input_test_peer(std::string stream_name)
        : work_(boost::asio::make_work_guard(io_)), acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_socket_(io_), stream_name_(std::move(stream_name))
    {
        client_socket_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        auto session = std::make_shared<rtmp_session>(std::move(connection), registry_);
        session->startup();
        runner_ = std::jthread([this]() { io_.run(); });

        const auto separator = stream_name_.find('/');
        require(separator != std::string::npos, "rtmp input stream name");
        const auto app = stream_name_.substr(0, separator);
        const auto stream = stream_name_.substr(separator + 1);
        rtmp_client_handler_t handler{};
        handler.send = &rtmp_input_test_peer::send_callback;
        const auto tc_url = "rtmp://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + '/' + app;
        client_ = rtmp_client_create(app.c_str(), stream.c_str(), tc_url.c_str(), this, &handler);
        require(client_ != nullptr, "rtmp input client");
        require(rtmp_client_start(client_, 0) == 0, "rtmp input client start");
        receive_until_started();
    }

    ~rtmp_input_test_peer()
    {
        rtmp_client_destroy(client_);
        client_ = nullptr;
        boost::system::error_code error;
        client_socket_.close(error);
        work_.reset();
        runner_.join();
    }

    void push_video_config(media_track track)
    {
        const auto packet = make_rtmp_video_sequence_header(std::move(track));
        require(rtmp_client_push_video(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push video config");
    }

    void push_audio_config(std::span<const std::uint8_t> asc)
    {
        std::vector<std::uint8_t> packet{0xaf, FLV_SEQUENCE_HEADER};
        packet.insert(packet.end(), asc.begin(), asc.end());
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push audio config");
    }

    void wait_track(const media_track& expected, std::uint64_t config_version)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto track = query_track(expected.id);
            if (track && track->kind == expected.kind && track->codec == expected.codec && track->clock_rate == expected.clock_rate &&
                track->channel_count == expected.channel_count && track->codec_config == expected.codec_config && track->config_version == config_version)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fail("rtmp input track update");
    }

    void wait_stream_removed()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!query_stream_exists())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fail("rtmp input stream removed");
    }

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
    {
        auto* self = static_cast<rtmp_input_test_peer*>(param);
        boost::system::error_code error;
        if (header_bytes != 0)
        {
            boost::asio::write(self->client_socket_, boost::asio::buffer(header, header_bytes), error);
        }
        if (!error && payload_bytes != 0)
        {
            boost::asio::write(self->client_socket_, boost::asio::buffer(payload, payload_bytes), error);
        }
        return error ? -1 : static_cast<int>(header_bytes + payload_bytes);
    }

    void receive_until_started()
    {
        std::array<std::uint8_t, 8 * 1024> data{};
        while (rtmp_client_getstate(client_) != RTMP_STATE_START)
        {
            const auto bytes = client_socket_.read_some(boost::asio::buffer(data));
            require(rtmp_client_input(client_, data.data(), bytes) == 0, "rtmp input client input");
        }
    }

    std::optional<media_track> query_track(track_id id)
    {
        std::promise<std::optional<media_track>> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [this, id, &promise]()
                          {
                              const auto stream = registry_.find(stream_name_);
                              if (stream)
                              {
                                  for (const auto& track : stream->tracks())
                                  {
                                      if (track.id == id)
                                      {
                                          promise.set_value(track);
                                          return;
                                      }
                                  }
                              }
                              promise.set_value(std::nullopt);
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtmp input track query");
        return future.get();
    }

    bool query_stream_exists()
    {
        std::promise<bool> promise;
        auto future = promise.get_future();
        boost::asio::post(io_, [this, &promise]() { promise.set_value(static_cast<bool>(registry_.find(stream_name_))); });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtmp input stream query");
        return future.get();
    }

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    stream_registry registry_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket client_socket_;
    std::string stream_name_;
    rtmp_client_t* client_{};
    std::jthread runner_;
};

class rtmp_output_test_peer final
{
   public:
    explicit rtmp_output_test_peer(media_track video_track = make_video_track(), bool with_audio = false)
        : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_socket_(io_), expected_video_codec_(video_track.codec)
    {
        stream_ = std::make_shared<media_stream>("live/camera");
        require(stream_->update_track(std::move(video_track)), "rtmp output video track");
        if (with_audio)
        {
            require(stream_->update_track(make_audio_track()), "rtmp output audio track");
        }
        require(registry_.add(stream_), "rtmp output registry add");

        client_socket_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        session_ = std::make_shared<rtmp_session>(std::move(connection), registry_);
        session_->startup();
        runner_ = std::jthread([this]() { io_.run(); });

        rtmp_client_handler_t handler{};
        handler.send = &rtmp_output_test_peer::send_callback;
        handler.onvideo = &rtmp_output_test_peer::video_callback;
        handler.onaudio = &rtmp_output_test_peer::audio_callback;
        handler.onscript = &rtmp_output_test_peer::media_callback;
        const auto tc_url = "rtmp://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/live";
        client_ = rtmp_client_create("live", "camera", tc_url.c_str(), this, &handler);
        require(client_ != nullptr, "rtmp output client");
        require(rtmp_client_start(client_, 2) == 0, "rtmp output client start");
        receive_until_video_config();
    }

    ~rtmp_output_test_peer()
    {
        rtmp_client_destroy(client_);
        client_ = nullptr;
        boost::system::error_code error;
        client_socket_.close(error);
        runner_.join();
    }

    rtmp_status pause()
    {
        require(rtmp_client_pause(client_, 1) == 0, "rtmp pause request");
        return read_status();
    }

    rtmp_status seek()
    {
        require(rtmp_client_seek(client_, 1'000.0) == 0, "rtmp seek request");
        return read_status();
    }

    void publish(media_frame frame)
    {
        boost::asio::post(io_, [stream = stream_, frame = std::move(frame)]() mutable { stream->publish(std::move(frame)); });
    }

    void receive_media(std::size_t count)
    {
        std::array<std::uint8_t, 8 * 1024> data{};
        while (media_order_.size() < count)
        {
            const auto bytes = client_socket_.read_some(boost::asio::buffer(data));
            require(rtmp_client_input(client_, data.data(), bytes) == 0, "rtmp output media input");
        }
    }

    [[nodiscard]] const std::vector<char>& media_order() const noexcept { return media_order_; }
    [[nodiscard]] std::size_t audio_config_count() const noexcept { return audio_config_count_; }

    void add_audio_track()
    {
        std::promise<bool> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [stream = stream_, &promise]() mutable { promise.set_value(stream->update_track(make_audio_track())); });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready && future.get(), "rtmp runtime add audio track");
    }

    void update_video_track(media_track track)
    {
        const auto expected = video_config_count_ + 1;
        std::promise<bool> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [stream = stream_, track = std::move(track), &promise]() mutable
                          { promise.set_value(stream->update_track(std::move(track))); });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready && future.get(), "rtmp output config reset");
        receive_until_video_config(expected);
    }

    void end_stream()
    {
        const std::weak_ptr<rtmp_session> weak = session_;
        boost::asio::post(io_, [stream = stream_]() { stream->end(); });
        session_.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!weak.expired() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(weak.expired(), "rtmp stream end releases session");
    }

    void disconnect_and_wait(bool with_media_write)
    {
        if (with_media_write)
        {
            publish(make_large_video_frame(0));
        }
        client_socket_.set_option(boost::asio::socket_base::linger(true, 0));
        boost::system::error_code error;
        client_socket_.close(error);

        const std::weak_ptr<rtmp_session> weak = session_;
        session_.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!weak.expired() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(weak.expired(), with_media_write ? "rtmp write error releases session" : "rtmp read error releases session");
    }

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
    {
        auto* self = static_cast<rtmp_output_test_peer*>(param);
        boost::system::error_code error;
        if (header_bytes != 0)
        {
            boost::asio::write(self->client_socket_, boost::asio::buffer(header, header_bytes), error);
        }
        if (!error && payload_bytes != 0)
        {
            boost::asio::write(self->client_socket_, boost::asio::buffer(payload, payload_bytes), error);
        }
        return error ? -1 : static_cast<int>(header_bytes + payload_bytes);
    }

    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
    {
        flv_video_tag_header_t video{};
        require(flv_video_tag_header_read(&video, static_cast<const std::uint8_t*>(data), bytes) > 0, "rtmp output video header");
        auto* self = static_cast<rtmp_output_test_peer*>(param);
        const auto expected = self->expected_video_codec_ == codec_id::h264 ? FLV_VIDEO_H264 : FLV_VIDEO_H265;
        require(video.codecid == expected, "rtmp output video codec");
        if (video.avpacket == FLV_SEQUENCE_HEADER)
        {
            ++self->video_config_count_;
        }
        else
        {
            self->media_order_.push_back('v');
        }
        return 0;
    }

    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
    {
        if (data != nullptr && bytes >= 2U)
        {
            auto* self = static_cast<rtmp_output_test_peer*>(param);
            if (static_cast<const std::uint8_t*>(data)[1] == FLV_SEQUENCE_HEADER)
            {
                ++self->audio_config_count_;
            }
            else
            {
                self->media_order_.push_back('a');
            }
        }
        return 0;
    }

    static int media_callback(void*, const void*, std::size_t, std::uint32_t) { return 0; }

    void receive_until_video_config()
    {
        receive_until_video_config(1);
    }

    void receive_until_video_config(std::size_t count)
    {
        std::array<std::uint8_t, 8 * 1024> data{};
        while (video_config_count_ < count)
        {
            const auto bytes = client_socket_.read_some(boost::asio::buffer(data));
            require(rtmp_client_input(client_, data.data(), bytes) == 0, "rtmp output client input");
        }
    }

    rtmp_status read_status()
    {
        std::array<std::uint8_t, 3> basic_header{};
        boost::asio::read(client_socket_, boost::asio::buffer(basic_header.data(), 1));
        const auto marker = static_cast<std::uint8_t>(basic_header[0] & 0x3fU);
        const std::size_t basic_bytes = marker == 0 ? 2U : (marker == 1 ? 3U : 1U);
        if (basic_bytes != 1U)
        {
            boost::asio::read(client_socket_, boost::asio::buffer(basic_header.data() + 1, basic_bytes - 1));
        }

        rtmp_chunk_header_t header{};
        require(rtmp_chunk_basic_header_read(basic_header.data(), &header.fmt, &header.cid) == static_cast<int>(basic_bytes),
                "rtmp status basic header");
        require(header.fmt == RTMP_CHUNK_TYPE_0, "rtmp status message header type");
        std::array<std::uint8_t, 11> message_header{};
        boost::asio::read(client_socket_, boost::asio::buffer(message_header));
        require(rtmp_chunk_message_header_read(message_header.data(), &header) == static_cast<int>(message_header.size()),
                "rtmp status message header");
        require(header.timestamp != 0x00ffffffU, "rtmp status extended timestamp");
        require(header.type == RTMP_TYPE_INVOKE && header.length <= 4'096U, "rtmp status invoke");

        std::vector<std::uint8_t> payload(header.length);
        boost::asio::read(client_socket_, boost::asio::buffer(payload));
        return parse_rtmp_status(payload);
    }

    boost::asio::io_context io_;
    stream_registry registry_;
    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket client_socket_;
    std::shared_ptr<rtmp_session> session_;
    rtmp_client_t* client_{};
    codec_id expected_video_codec_{};
    std::size_t video_config_count_{};
    std::size_t audio_config_count_{};
    std::vector<char> media_order_;
    std::jthread runner_;
};

void test_rtmp_input_codec_configuration_updates()
{
    {
        rtmp_input_test_peer peer("live/h264-config");
        const auto initial = make_video_track();
        peer.push_video_config(initial);
        peer.wait_track(initial, 1);
        auto updated = make_video_track();
        updated.codec_config = h264_config_updated;
        peer.push_video_config(updated);
        peer.wait_track(updated, 2);
    }

    {
        rtmp_input_test_peer peer("live/h265-config");
        const auto initial = make_h265_track();
        peer.push_video_config(initial);
        peer.wait_track(initial, 1);
        auto updated = make_h265_track();
        updated.codec_config = h265_config_updated;
        peer.push_video_config(updated);
        peer.wait_track(updated, 2);
    }

    {
        rtmp_input_test_peer peer("live/aac-config");
        const auto initial = make_audio_track();
        peer.push_audio_config(initial.codec_config);
        peer.wait_track(initial, 1);
        auto updated = make_audio_track();
        updated.clock_rate = 48'000;
        updated.codec_config = aac_asc_updated;
        peer.push_audio_config(updated.codec_config);
        peer.wait_track(updated, 2);
    }
}

void test_rtmp_input_rejects_video_codec_change()
{
    {
        rtmp_input_test_peer peer("live/h264-switch");
        const auto h264 = make_video_track();
        peer.push_video_config(h264);
        peer.wait_track(h264, 1);
        peer.push_video_config(make_h265_track());
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/h265-switch");
        const auto h265 = make_h265_track();
        peer.push_video_config(h265);
        peer.wait_track(h265, 1);
        peer.push_video_config(make_video_track());
        peer.wait_stream_removed();
    }
}

void test_rtmp_rejects_live_playback_control()
{
    rtmp_output_test_peer peer;
    const auto pause = peer.pause();
    const auto seek = peer.seek();
    require(pause.level == "error" && pause.code == "NetStream.Pause.Failed", "rtmp live pause rejected");
    require(seek.level == "error" && seek.code == "NetStream.Seek.Failed", "rtmp live seek rejected");
}

void test_rtmp_output_pull_codecs_and_order()
{
    {
        rtmp_output_test_peer peer;
        peer.publish(make_video_frame(0, true));
        peer.receive_media(1);
        require(peer.media_order() == std::vector<char>{'v'}, "rtmp h264 pull media");
    }

    {
        rtmp_output_test_peer peer(make_h265_track());
        peer.publish(make_h265_frame(0, true));
        peer.receive_media(1);
        require(peer.media_order() == std::vector<char>{'v'}, "rtmp h265 pull media");
    }

    {
        rtmp_output_test_peer peer(make_video_track(), true);
        peer.publish(make_video_frame(0, true));
        peer.publish(make_audio_frame(20'000'000));
        peer.publish(make_video_frame(40'000'000, false));
        peer.publish(make_audio_frame(60'000'000));
        peer.receive_media(4);
        require(peer.media_order() == std::vector<char>({'v', 'a', 'v', 'a'}), "rtmp audio video pull order");
    }
}

void test_rtmp_output_config_reset_and_end()
{
    rtmp_output_test_peer peer;
    auto updated = make_video_track();
    updated.codec_config = h264_config_updated;
    peer.update_video_track(std::move(updated));
    peer.publish(make_video_frame(0, true));
    peer.receive_media(1);
    peer.end_stream();
}

void test_rtmp_output_runtime_add_audio()
{
    rtmp_output_test_peer peer;
    peer.publish(make_video_frame(0, true));
    peer.receive_media(1);
    peer.add_audio_track();
    peer.publish(make_video_frame(40'000'000, true));
    peer.publish(make_audio_frame(60'000'000));
    peer.receive_media(3);
    require(peer.audio_config_count() == 1, "rtmp runtime audio sequence header");
    require(peer.media_order() == std::vector<char>({'v', 'v', 'a'}), "rtmp runtime add audio keeps media flowing");
}

void test_rtmp_tcp_error_lifecycle()
{
    {
        rtmp_output_test_peer peer;
        peer.disconnect_and_wait(false);
    }
    {
        rtmp_output_test_peer peer;
        peer.disconnect_and_wait(true);
    }
}

void test_tcp_listener_startup_error()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor occupied(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

    io_context_pool workers(1);
    tcp_listener listener(workers, occupied.local_endpoint().port(), [](boost::asio::ip::tcp::socket) {});
    require(static_cast<bool>(listener.startup()), "tcp listener reports bind failure");
}


void test_tcp_connection_shutdown_lifecycle()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto server = acceptor.accept();

    int io_callback_count = 0;
    auto connection = std::make_shared<tcp_connection>(std::move(server));
    connection->startup(
        [&io_callback_count](boost::system::error_code, std::span<const std::uint8_t>) { ++io_callback_count; },
        [&io_callback_count](boost::system::error_code, std::size_t) { ++io_callback_count; });
    const std::weak_ptr<tcp_connection> weak_connection = connection;

    connection->shutdown();
    connection->shutdown();
    connection.reset();
    require(!weak_connection.expired(), "tcp connection shutdown keeps self until owner worker cleanup");

    io.run();

    require(io_callback_count == 0, "tcp shutdown suppresses cancellation callbacks");
    require(weak_connection.expired(), "tcp connection released after owner worker cleanup");

    boost::system::error_code error;
    client.close(error);
}

void test_tcp_connection_io_error_propagation()
{
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
        boost::asio::ip::tcp::socket client(io);
        client.connect(acceptor.local_endpoint());
        auto connection = std::make_shared<tcp_connection>(acceptor.accept());
        boost::system::error_code read_error;
        connection->startup(
            [&read_error](boost::system::error_code error, std::span<const std::uint8_t>) { read_error = error; }, {});
        client.close();
        io.run();
        require(static_cast<bool>(read_error), "tcp connection reports read error");
        connection->shutdown();
        io.restart();
        io.run();
    }

    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
        boost::asio::ip::tcp::socket client(io);
        client.connect(acceptor.local_endpoint());
        auto connection = std::make_shared<tcp_connection>(acceptor.accept());
        boost::system::error_code write_error;
        connection->startup({}, [&write_error](boost::system::error_code error, std::size_t) { write_error = error; });

        client.set_option(boost::asio::socket_base::linger(true, 0));
        client.close();
        std::vector<std::uint8_t> data(8 * 1024 * 1024, 0x5a);
        connection->write(data);
        io.run();
        require(static_cast<bool>(write_error), "tcp connection reports write error");
        connection->shutdown();
        io.restart();
        io.run();
    }
}

void test_tcp_connection_shutdown_discards_pending_writes()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto connection = std::make_shared<tcp_connection>(acceptor.accept());
    connection->startup({}, {});
    const std::weak_ptr<tcp_connection> weak_connection = connection;

    std::vector<std::uint8_t> data(8 * 1024 * 1024, 0x5a);
    connection->write(data);
    connection->write(data);
    connection->shutdown();
    connection.reset();
    io.run();

    require(weak_connection.expired(), "tcp shutdown releases connection without draining writes");
    boost::system::error_code error;
    client.close(error);
}

void test_tcp_listener_worker_affinity()
{
    io_context_pool workers(2);
    boost::asio::ip::tcp::acceptor probe(workers.context(0), boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
    const auto port = probe.local_endpoint().port();
    probe.close();

    std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::weak_ptr<tcp_listener> weak_listener;
    auto listener = std::make_shared<tcp_listener>(
        workers,
        port,
        [&](boost::asio::ip::tcp::socket socket)
        {
            bool complete = false;
            {
                std::scoped_lock lock(mutex);
                threads.push_back(std::this_thread::get_id());
                complete = threads.size() == 2U;
            }
            boost::system::error_code error;
            socket.close(error);
            if (complete)
            {
                boost::asio::post(workers.context(0),
                                  [&]()
                                  {
                                      if (const auto current = weak_listener.lock())
                                      {
                                          current->shutdown();
                                      }
                                      workers.release_work();
                                  });
            }
        });
    weak_listener = listener;
    require(!listener->startup(), "tcp listener worker startup");

    boost::asio::io_context client_io;
    boost::asio::ip::tcp::socket first(client_io);
    boost::asio::ip::tcp::socket second(client_io);
    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), port);
    first.connect(endpoint);
    second.connect(endpoint);

    workers.run();

    std::scoped_lock lock(mutex);
    require(threads.size() == 2U, "tcp listener accepted clients");
    require(threads[0] != threads[1], "tcp listener assigns different workers");
}

void start_http_flv_client(boost::asio::ip::tcp::socket& client, std::string_view path)
{
    const auto request = "GET " + std::string(path) +
                         " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Connection: close\r\n\r\n";
    boost::asio::write(client, boost::asio::buffer(request));

    boost::asio::streambuf response;
    boost::asio::read_until(client, response, "\r\n\r\n");
    std::istream input(&response);
    std::string status;
    std::getline(input, status);
    require(status.starts_with("HTTP/1.1 200"), "http flv response status");
}

void drain_http_flv_client(boost::asio::ip::tcp::socket& client)
{
    boost::system::error_code error;
    auto quiet_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    std::array<std::uint8_t, 4096> buffer{};
    while (std::chrono::steady_clock::now() < quiet_deadline)
    {
        const auto available = client.available(error);
        require(!error, "http flv client drain");
        if (available == 0U)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const auto bytes = client.read_some(boost::asio::buffer(buffer.data(), std::min(buffer.size(), available)), error);
        require(!error && bytes > 0U, "http flv initial output");
        quiet_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    }
}

void require_http_session_released(const std::weak_ptr<http_session>& session, std::string_view message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!session.expired() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(session.expired(), message);
}

void test_http_flv_client_disconnect()
{
    boost::asio::io_context io;
    stream_registry registry;
    hls_service hls(registry);
    whep_service whep(registry, boost::asio::ip::make_address("127.0.0.1"));

    auto stream = std::make_shared<media_stream>("live/http-flv-disconnect");
    require(stream->update_track(make_video_track()), "http flv disconnect track");
    require(registry.add(stream), "http flv disconnect stream");

    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto session = std::make_shared<http_session>(acceptor.accept(), registry, hls, whep);
    const std::weak_ptr<http_session> weak_session = session;
    session->startup();
    session.reset();

    std::jthread runner([&io]() { io.run(); });
    start_http_flv_client(client, "/live/http-flv-disconnect.flv");
    drain_http_flv_client(client);

    boost::asio::post(io, [stream, frame = make_large_video_frame(0)]() mutable { stream->publish(std::move(frame)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    boost::system::error_code error;
    client.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    client.close(error);

    require_http_session_released(weak_session, "http flv disconnect releases session");
    runner.join();
}

void test_http_flv_stream_end_during_write()
{
    boost::asio::io_context io;
    stream_registry registry;
    hls_service hls(registry);
    whep_service whep(registry, boost::asio::ip::make_address("127.0.0.1"));

    auto stream = std::make_shared<media_stream>("live/http-flv-end-write");
    require(stream->update_track(make_video_track()), "http flv end write track");
    require(registry.add(stream), "http flv end write stream");

    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto session = std::make_shared<http_session>(acceptor.accept(), registry, hls, whep);
    const std::weak_ptr<http_session> weak_session = session;
    session->startup();
    session.reset();

    std::jthread runner([&io]() { io.run(); });
    start_http_flv_client(client, "/live/http-flv-end-write.flv");
    drain_http_flv_client(client);

    boost::asio::post(io, [stream, frame = make_large_video_frame(0)]() mutable { stream->publish(std::move(frame)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    boost::asio::post(io, [stream]() { stream->end(); });

    require_http_session_released(weak_session, "http flv stream end closes in-flight write");
    boost::system::error_code error;
    client.close(error);
    runner.join();
}

void test_http_flv_pending_bootstrap_end()
{
    boost::asio::io_context io;
    stream_registry registry;
    hls_service hls(registry);
    whep_service whep(registry, boost::asio::ip::make_address("127.0.0.1"));

    auto stream = std::make_shared<media_stream>("live/http-flv-pending-end");
    require(stream->update_track(make_video_track()), "http flv pending end track");
    require(registry.add(stream), "http flv pending end stream");

    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto session = std::make_shared<http_session>(acceptor.accept(), registry, hls, whep);
    const std::weak_ptr<http_session> weak_session = session;
    session->startup();
    session.reset();

    std::jthread runner([&io]() { io.run(); });
    start_http_flv_client(client, "/live/http-flv-pending-end.flv");
    drain_http_flv_client(client);

    boost::asio::post(io, [stream, frame = make_large_video_frame(0)]() mutable { stream->publish(std::move(frame)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    boost::asio::post(io,
                      [stream]()
                      {
                          auto track = make_video_track();
                          track.codec_config = h264_config_updated;
                          require(stream->update_track(std::move(track)), "http flv pending generation reset");
                      });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    boost::asio::post(io, [stream]() { stream->end(); });

    require_http_session_released(weak_session, "http flv pending bootstrap end closes session");
    boost::system::error_code error;
    client.close(error);
    runner.join();
}

void test_http_flv_pull_one_frame_and_overrun()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-pull");
    require(stream->update_track(make_video_track()), "http flv pull video track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&capture]() { ++capture.ends; });
    static_cast<void>(stream->add_reader(output, reader_worker.get_executor()));
    drain();

    require(capture.writes.size() == 1U && capture.writes.front().bootstrap, "http flv bootstrap is first logical write");
    require(capture.writes.front().data.size() >= 9U &&
                std::string_view(reinterpret_cast<const char*>(capture.writes.front().data.data()), 3) == "FLV",
            "http flv bootstrap contains file header");
    require((capture.writes.front().data[4] & 0x01U) != 0U && (capture.writes.front().data[4] & 0x04U) == 0U,
            "http flv video-only header flags");

    stream->publish(make_video_frame(0, true));
    stream->publish(make_video_frame(40'000'000, false));
    drain();
    require(capture.writes.size() == 1U, "http flv waits bootstrap completion before first read");

    output->write_complete(1);
    drain();
    require(capture.writes.size() == 2U && !capture.writes.back().bootstrap, "http flv reads one frame after bootstrap");

    stream->publish(make_video_frame(1'000'000'000, true));
    stream->publish(make_video_frame(1'040'000'000, false));
    stream->publish(make_video_frame(2'000'000'000, true));
    drain();
    require(capture.writes.size() == 2U, "http flv keeps no frame backlog while write is pending");

    output->write_complete(1);
    drain();
    require(capture.writes.size() == 3U, "http flv pulls next frame after write completion");
    const auto decoded = demux_http_flv(capture);
    std::vector<std::int64_t> video_pts;
    for (const auto& packet : decoded.packets)
    {
        if (packet.codec == FLV_VIDEO_H264)
        {
            video_pts.push_back(packet.pts);
        }
    }
    require(video_pts == std::vector<std::int64_t>{0, 2'000}, "slow http flv reader resyncs to current gop key frame");
}

void test_http_flv_h265_pull()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-h265");
    require(stream->update_track(make_h265_track()), "http flv h265 track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&capture]() { ++capture.ends; });
    static_cast<void>(stream->add_reader(output, reader_worker.get_executor()));
    drain();
    require(capture.writes.size() == 1U && capture.writes.front().bootstrap, "http flv h265 bootstrap");

    output->write_complete(1);
    stream->publish(make_h265_frame(0, true));
    drain();
    require(capture.writes.size() == 2U && !capture.writes.back().bootstrap, "http flv h265 media write");

    const auto decoded = demux_http_flv(capture);
    require(std::ranges::any_of(decoded.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_HVCC; }),
            "http flv h265 config packet");
    const auto media = std::ranges::find_if(decoded.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_H265; });
    require(media != decoded.packets.end() && media->flags == 1 && media->pts == 0 && !media->payload.empty(), "http flv h265 key frame");
}

void test_http_flv_fast_and_slow_readers()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-fast-slow");
    require(stream->update_track(make_video_track()), "http flv fast slow track");
    http_flv_capture fast_capture;
    http_flv_capture slow_capture;
    auto fast = std::make_shared<http_flv_output>(
        [&fast_capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { fast_capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&fast_capture]() { ++fast_capture.ends; });
    auto slow = std::make_shared<http_flv_output>(
        [&slow_capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { slow_capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&slow_capture]() { ++slow_capture.ends; });
    static_cast<void>(stream->add_reader(fast, reader_worker.get_executor()));
    static_cast<void>(stream->add_reader(slow, reader_worker.get_executor()));
    drain();
    fast->write_complete(1);
    slow->write_complete(1);

    stream->publish(make_video_frame(0, true));
    drain();
    const std::array<std::pair<std::int64_t, bool>, 4> frames{
        std::pair{40'000'000LL, false},
        std::pair{1'000'000'000LL, true},
        std::pair{1'040'000'000LL, false},
        std::pair{2'000'000'000LL, true},
    };
    for (const auto& [pts, key_frame] : frames)
    {
        fast->write_complete(1);
        stream->publish(make_video_frame(pts, key_frame));
        drain();
    }

    require(fast_capture.writes.size() == 6U, "fast http flv reader receives every frame");
    require(slow_capture.writes.size() == 2U, "slow http flv reader holds only current frame write");
    slow->write_complete(1);
    drain();
    require(slow_capture.writes.size() == 3U, "slow http flv reader resumes independently");

    const auto fast_packets = demux_http_flv(fast_capture);
    const auto slow_packets = demux_http_flv(slow_capture);
    require(std::ranges::count_if(fast_packets.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_H264; }) == 5,
            "fast http flv media count");
    const auto slow_video = std::ranges::find_if(
        slow_packets.packets.rbegin(), slow_packets.packets.rend(), [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_H264; });
    require(slow_video != slow_packets.packets.rend() && slow_video->pts == 2'000 && slow_video->flags == 1,
            "slow http flv reader resumes at latest key frame");
}

void test_http_flv_audio_video_order()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-av");
    require(stream->update_track(make_video_track()), "http flv av video track");
    require(stream->update_track(make_audio_track()), "http flv av audio track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&capture]() { ++capture.ends; });
    static_cast<void>(stream->add_reader(output, reader_worker.get_executor()));
    drain();
    require((capture.writes.front().data[4] & 0x05U) == 0x05U, "http flv audio video header flags");
    output->write_complete(1);

    stream->publish(make_video_frame(0, true));
    stream->publish(make_audio_frame(20'000'000));
    stream->publish(make_video_frame(40'000'000, false));
    stream->publish(make_audio_frame(60'000'000));
    drain();
    for (std::size_t index = 1; index < 4; ++index)
    {
        output->write_complete(1);
        drain();
    }

    const auto decoded = demux_http_flv(capture);
    std::vector<int> codecs;
    for (const auto& packet : decoded.packets)
    {
        if (packet.codec == FLV_VIDEO_H264 || packet.codec == FLV_AUDIO_AAC)
        {
            codecs.push_back(packet.codec);
        }
    }
    require(codecs == std::vector<int>{FLV_VIDEO_H264, FLV_AUDIO_AAC, FLV_VIDEO_H264, FLV_AUDIO_AAC},
            "http flv preserves audio video frame order");
}

void test_http_flv_track_reset()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-reset");
    require(stream->update_track(make_video_track()), "http flv reset initial track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](media_reader_generation generation, std::vector<std::uint8_t> data, bool bootstrap)
        { capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&capture]() { ++capture.ends; });
    static_cast<void>(stream->add_reader(output, reader_worker.get_executor()));
    drain();

    auto updated_video = make_video_track();
    updated_video.codec_config = h264_config_updated;
    require(stream->update_track(std::move(updated_video)), "http flv reset video config");
    drain();
    require(capture.writes.size() == 2U && capture.writes.back().bootstrap && capture.writes.back().generation == 2,
            "http flv reset emits new generation bootstrap");
    require(capture.ends == 0U, "http flv same topology reset stays open");

    require(stream->update_track(make_audio_track()), "http flv reset topology change");
    drain();
    require(capture.ends == 1U, "http flv topology change closes once");
}

void test_rtsp_pull_url_contract()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

    boost::asio::io_context client_io;
    stream_registry registry;
    auto invalid = std::make_shared<rtsp_input_session>(client_io, registry, "relay/invalid", "rtsp://127.0.0.1:99999/live/test");
    require(!invalid->startup(), "rtsp invalid port rejected");
    require(!registry.find("relay/invalid"), "rtsp invalid url leaves registry unchanged");

    const auto port = acceptor.local_endpoint().port();
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/test";
    const auto credential_url = "rtsp://us%65r:p%40ss@127.0.0.1:" + std::to_string(port) + "/live/test";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/auth", credential_url);
    const std::weak_ptr<rtsp_input_session> weak_pull = pull;
    require(pull->startup(), "rtsp auth pull startup");
    pull.reset();

    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);

    const auto first = read_rtsp_headers(socket);
    require(first.starts_with("DESCRIBE " + request_url + " RTSP/1.0\r\n"), "rtsp auth sanitized request uri");
    require(first.find("user") == std::string::npos, "rtsp auth request hides username");
    require(first.find("p@ss") == std::string::npos, "rtsp auth request hides password");
    const auto first_cseq = rtsp_header_value(first, "CSeq:");
    require(!first_cseq.empty(), "rtsp auth first cseq");

    const auto unauthorized =
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: " +
        first_cseq +
        "\r\n"
        "WWW-Authenticate: Basic realm=\"media_server_test\"\r\n"
        "Content-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(unauthorized));

    const auto second = read_rtsp_headers(socket);
    require(second.starts_with("DESCRIBE " + request_url + " RTSP/1.0\r\n"), "rtsp auth retry request uri");
    require(second.find("Authorization: Basic dXNlcjpwQHNz\r\n") != std::string::npos, "rtsp auth credentials");
    const auto second_cseq = rtsp_header_value(second, "CSeq:");
    require(!second_cseq.empty(), "rtsp auth second cseq");

    const auto not_found =
        "RTSP/1.0 404 Not Found\r\n"
        "CSeq: " +
        second_cseq +
        "\r\n"
        "Content-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(not_found));
    boost::system::error_code error;
    socket.close(error);
    runner.join();

    require(!registry.find("relay/auth"), "rtsp auth failed pull removes stream");
    require(weak_pull.expired(), "rtsp pull releases itself after shutdown");
}

void test_rtsp_input_selects_single_audio_and_video()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/multi";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/single-av", request_url);
    require(pull->startup(), "rtsp single audio video pull startup");
    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);

    const auto describe = read_rtsp_headers(socket);
    const auto describe_cseq = rtsp_header_value(describe, "CSeq:");
    const auto sdp = std::string("v=0\r\n") +
                     "o=- 0 0 IN IP4 127.0.0.1\r\n"
                     "s=test\r\n"
                     "c=IN IP4 127.0.0.1\r\n"
                     "t=0 0\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 VP8/90000\r\n"
                     "a=control:vp8\r\n"
                     "m=video 0 RTP/AVP 97\r\n"
                     "a=rtpmap:97 H265/90000\r\n"
                     "a=control:h265\r\n"
                     "m=video 0 RTP/AVP 98\r\n"
                     "a=rtpmap:98 H264/90000\r\n"
                     "a=control:h264\r\n"
                     "m=audio 0 RTP/AVP 99\r\n"
                     "a=rtpmap:99 opus/48000/2\r\n"
                     "a=control:opus\r\n"
                     "m=audio 0 RTP/AVP 100\r\n"
                     "a=rtpmap:100 MPEG4-GENERIC/44100/2\r\n"
                     "a=fmtp:100 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n"
                     "a=control:aac-first\r\n"
                     "m=audio 0 RTP/AVP 101\r\n"
                     "a=rtpmap:101 MPEG4-GENERIC/48000/2\r\n"
                     "a=fmtp:101 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1190\r\n"
                     "a=control:aac-second\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + describe_cseq + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto video_setup = read_rtsp_headers(socket);
    require(video_setup.starts_with("SETUP " + request_url + "/h265 RTSP/1.0\r\n"), "rtsp selects first supported video");
    require(video_setup.find("interleaved=0-1") != std::string::npos, "rtsp selected video channels");
    const auto video_cseq = rtsp_header_value(video_setup, "CSeq:");
    const auto video_response = "RTSP/1.0 200 OK\r\nCSeq: " + video_cseq +
                                "\r\nSession: single-av;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(video_response));

    const auto audio_setup = read_rtsp_headers(socket);
    require(audio_setup.starts_with("SETUP " + request_url + "/aac-first RTSP/1.0\r\n"), "rtsp selects first supported audio");
    require(audio_setup.find("interleaved=2-3") != std::string::npos, "rtsp selected audio channels");
    const auto audio_cseq = rtsp_header_value(audio_setup, "CSeq:");
    const auto audio_response = "RTSP/1.0 200 OK\r\nCSeq: " + audio_cseq +
                                "\r\nSession: single-av;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(audio_response));

    const auto play = read_rtsp_headers(socket);
    require(play.starts_with("PLAY ") && play.find(request_url) != std::string::npos, "rtsp skips duplicate media before play");
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: single-av;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));
    boost::system::error_code error;
    socket.close(error);
    runner.join();
    require(!registry.find("relay/single-av"), "rtsp single audio video pull closes");
}

void test_rtsp_input_media_driven_keepalive()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/keepalive";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/keepalive", request_url);
    require(pull->startup(), "rtsp keepalive pull startup");
    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);

    const auto describe = read_rtsp_headers(socket);
    constexpr std::string_view sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" +
                                   std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto setup = read_rtsp_headers(socket);
    const auto setup_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(setup, "CSeq:") +
                                "\r\nSession: keepalive-session;timeout=2\r\n"
                                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(setup_response));

    const auto play = read_rtsp_headers(socket);
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: keepalive-session;timeout=2\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
    constexpr std::array<std::uint8_t, 21> interleaved_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::write(socket, boost::asio::buffer(interleaved_rtp));

    const auto options = read_rtsp_headers_until(socket, std::chrono::seconds(1));
    require(options.starts_with("OPTIONS * RTSP/1.0\r\n"), "rtsp media drives keepalive options");
    require(options.find("Session: keepalive-session\r\n") != std::string::npos, "rtsp keepalive carries session");
    const auto options_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(options, "CSeq:") + "\r\nPublic: OPTIONS\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(options_response));

    boost::system::error_code error;
    socket.close(error);
    runner.join();
    require(!registry.find("relay/keepalive"), "rtsp keepalive pull closes");
}

class rtsp_output_test_peer final
{
   public:
    explicit rtsp_output_test_peer(bool h265 = false) : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_(io_)
    {
        stream_ = std::make_shared<media_stream>("live/test", io_.get_executor());
        require(stream_->update_track(h265 ? make_h265_track() : make_video_track()), "rtsp output video track");
        require(stream_->update_track(make_audio_track()), "rtsp output audio track");
        require(registry_.add(stream_), "rtsp output registry add");

        client_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        auto session = std::make_shared<rtsp_output_session>(std::move(connection), registry_, acceptor_.local_endpoint().port());
        session_ = session;
        session->startup();
        runner_ = std::jthread([this]() { io_.run(); });
    }

    ~rtsp_output_test_peer()
    {
        boost::system::error_code error;
        client_.close(error);
        runner_.join();
    }

    std::string request(std::string_view request)
    {
        boost::asio::write(client_, boost::asio::buffer(request));
        boost::asio::read_until(client_, boost::asio::dynamic_buffer(read_buffer_), "\r\n\r\n");
        const auto header_end = read_buffer_.find("\r\n\r\n");
        require(header_end != std::string::npos, "rtsp response header");
        const auto total = header_end + 4U + rtsp_content_length(read_buffer_);
        if (read_buffer_.size() < total)
        {
            boost::asio::read(client_, boost::asio::dynamic_buffer(read_buffer_), boost::asio::transfer_exactly(total - read_buffer_.size()));
        }
        auto response = read_buffer_.substr(0, total);
        read_buffer_.erase(0, total);
        return response;
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    bool update_track(media_track track)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        boost::asio::post(io_,
                          [stream = stream_, track = std::move(track), promise]() mutable
                          {
                              promise->set_value(stream->update_track(std::move(track)));
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtsp output track update");
        return future.get();
    }

    void publish(media_frame frame)
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        boost::asio::post(io_,
                          [stream = stream_, frame = std::move(frame), promise]() mutable
                          {
                              stream->publish(std::move(frame));
                              promise->set_value();
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtsp output publish");
        future.get();
    }

    [[nodiscard]] bool session_alive() const { return !session_.expired(); }

    std::optional<rtsp_interleaved_packet> read_interleaved(std::chrono::steady_clock::duration timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto wait_for_bytes = [this, deadline](std::size_t size)
        {
            boost::system::error_code error;
            while (read_buffer_.size() < size && std::chrono::steady_clock::now() < deadline)
            {
                const auto available = client_.available(error);
                if (error)
                {
                    return false;
                }
                if (available == 0U)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                const auto old_size = read_buffer_.size();
                read_buffer_.resize(old_size + available);
                const auto bytes = client_.read_some(boost::asio::buffer(read_buffer_.data() + old_size, available), error);
                if (error)
                {
                    read_buffer_.resize(old_size);
                    return false;
                }
                read_buffer_.resize(old_size + bytes);
            }
            return read_buffer_.size() >= size;
        };
        if (!wait_for_bytes(4U))
        {
            return std::nullopt;
        }

        const auto* header = reinterpret_cast<const std::uint8_t*>(read_buffer_.data());
        require(header[0] == 0x24, "rtsp interleaved marker");
        const auto size = (static_cast<std::size_t>(header[2]) << 8U) | static_cast<std::size_t>(header[3]);
        require(size > 0U, "rtsp interleaved payload size");
        if (!wait_for_bytes(4U + size))
        {
            return std::nullopt;
        }
        const auto channel = static_cast<std::uint8_t>(read_buffer_[1]);
        std::vector<std::uint8_t> payload(size);
        std::memcpy(payload.data(), read_buffer_.data() + 4, size);
        read_buffer_.erase(0, 4U + size);
        return rtsp_interleaved_packet{.channel = channel, .payload = std::move(payload)};
    }

   private:
    boost::asio::io_context io_;
    stream_registry registry_;
    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket client_;
    std::weak_ptr<rtsp_output_session> session_;
    std::string read_buffer_;
    std::jthread runner_;
};

void test_rtsp_output_session_contract()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";

    const auto options = peer.request("OPTIONS * RTSP/1.0\r\n"
                                      "CSeq: 1\r\n\r\n");
    require(options.starts_with("RTSP/1.0 200"), "rtsp output options");
    require(rtsp_header_value(options, "Public:") == "OPTIONS,DESCRIBE,SETUP,TEARDOWN,PLAY,GET_PARAMETER",
            "rtsp output advertised methods");

    const auto pause = peer.request("PAUSE " + base +
                                    " RTSP/1.0\r\n"
                                    "CSeq: 2\r\n\r\n");
    require(pause.starts_with("RTSP/1.0 501"), "rtsp output pause unsupported");

    const auto set_parameter = peer.request("SET_PARAMETER " + base +
                                            " RTSP/1.0\r\n"
                                            "CSeq: 3\r\n"
                                            "Content-Length: 0\r\n\r\n");
    require(set_parameter.starts_with("RTSP/1.0 501"), "rtsp output set parameter unsupported");

    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 4\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp output describe");
    require(describe.find("a=control:trackID=1\r\n") != std::string::npos, "rtsp output video control");
    require(describe.find("a=control:trackID=2\r\n") != std::string::npos, "rtsp output audio control");

    const auto wrong_stream = peer.request("SETUP rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                           "/live/other/trackID=1 RTSP/1.0\r\n"
                                           "CSeq: 5\r\n"
                                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(wrong_stream.starts_with("RTSP/1.0 404"), "rtsp output setup stream identity");

    const auto video_setup = peer.request("SETUP " + base +
                                          "/trackID=1 RTSP/1.0\r\n"
                                          "CSeq: 6\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(video_setup.starts_with("RTSP/1.0 200"), "rtsp output video setup");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(!session.empty(), "rtsp output session id");

    const auto duplicate_setup = peer.request("SETUP " + base +
                                              "/trackID=1 RTSP/1.0\r\n"
                                              "CSeq: 7\r\n"
                                              "Session: " +
                                              session +
                                              "\r\n"
                                              "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(duplicate_setup.starts_with("RTSP/1.0 200"), "rtsp output idempotent setup");

    const auto wrong_session = peer.request("SETUP " + base +
                                            "/trackID=2 RTSP/1.0\r\n"
                                            "CSeq: 8\r\n"
                                            "Session: wrong\r\n"
                                            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(wrong_session.starts_with("RTSP/1.0 454"), "rtsp output setup session identity");

    const auto channel_conflict = peer.request("SETUP " + base +
                                               "/trackID=2 RTSP/1.0\r\n"
                                               "CSeq: 9\r\n"
                                               "Session: " +
                                               session +
                                               "\r\n"
                                               "Transport: RTP/AVP/TCP;unicast;interleaved=1-2\r\n\r\n");
    require(channel_conflict.starts_with("RTSP/1.0 461"), "rtsp output interleaved channel conflict");

    const auto audio_setup = peer.request("SETUP " + base +
                                          "/trackID=2 RTSP/1.0\r\n"
                                          "CSeq: 10\r\n"
                                          "Session: " +
                                          session +
                                          "\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(audio_setup.starts_with("RTSP/1.0 200"), "rtsp output audio setup");

    const auto wrong_play = peer.request("PLAY rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                         "/live/other RTSP/1.0\r\n"
                                         "CSeq: 11\r\n"
                                         "Session: " +
                                         session + "\r\n\r\n");
    require(wrong_play.starts_with("RTSP/1.0 404"), "rtsp output play stream identity");

    const auto play = peer.request("PLAY " + base +
                                   " RTSP/1.0\r\n"
                                   "CSeq: 12\r\n"
                                   "Session: " +
                                   session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp output play");

    const auto late_setup = peer.request("SETUP " + base +
                                         "/trackID=1 RTSP/1.0\r\n"
                                         "CSeq: 13\r\n"
                                         "Session: " +
                                         session +
                                         "\r\n"
                                         "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(late_setup.starts_with("RTSP/1.0 455"), "rtsp output reject setup after play");
}

void test_rtsp_output_h265()
{
    rtsp_output_test_peer peer(true);
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp h265 describe");
    require(describe.find("H265/90000") != std::string::npos, "rtsp h265 rtpmap");
    require(describe.find("sprop-vps=") != std::string::npos, "rtsp h265 vps");
    require(describe.find("sprop-sps=") != std::string::npos, "rtsp h265 sps");
    require(describe.find("sprop-pps=") != std::string::npos, "rtsp h265 pps");

    const auto setup = peer.request("SETUP " + base +
                                    "/trackID=1 RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(setup.starts_with("RTSP/1.0 200"), "rtsp h265 setup");
    const auto session = rtsp_header_value(setup, "Session:");
    require(!session.empty(), "rtsp h265 session");

    const auto frame = make_h265_frame(0, true);
    peer.publish(frame);

    const auto play = peer.request("PLAY " + base +
                                   " RTSP/1.0\r\n"
                                   "CSeq: 3\r\n"
                                   "Session: " +
                                   session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp h265 play");

    h265_rtp_capture capture;
    rtp_payload_t handler{
        .alloc = nullptr,
        .free = nullptr,
        .packet = &capture_h265_nalu,
    };
    const auto decoder = std::unique_ptr<void, decltype(&rtp_payload_decode_destroy)>(rtp_payload_decode_create(96, "H265", &handler, &capture),
                                                                                      &rtp_payload_decode_destroy);
    require(decoder != nullptr, "rtsp h265 depacketizer create");

    bool marker = false;
    while (!marker)
    {
        const auto interleaved = peer.read_interleaved(std::chrono::seconds(1));
        require(interleaved.has_value(), "rtsp h265 interleaved packet");
        if (interleaved->channel == 1U)
        {
            continue;
        }
        require(interleaved->channel == 0U, "rtsp h265 rtp channel");
        rtp_packet_t packet{};
        require(rtp_packet_deserialize(&packet, interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) == 0,
                "rtsp h265 rtp packet");
        require(packet.rtp.pt == 96U, "rtsp h265 rtp payload type");
        require(rtp_payload_decode_input(decoder.get(), interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) >= 0,
                "rtsp h265 depacketize");
        marker = packet.rtp.m != 0;
    }
    require(capture.access_unit == *frame.payload, "rtsp h265 cached access unit");
}

void test_rtsp_output_setup_track_lifecycle()
{
    {
        rtsp_output_test_peer peer(true);
        const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
        const auto describe = peer.request("DESCRIBE " + base +
                                           " RTSP/1.0\r\n"
                                           "CSeq: 1\r\n"
                                           "Accept: application/sdp\r\n\r\n");
        require(describe.starts_with("RTSP/1.0 200"), "rtsp video only describe");

        const auto setup = peer.request("SETUP " + base +
                                        "/trackID=1 RTSP/1.0\r\n"
                                        "CSeq: 2\r\n"
                                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
        require(setup.starts_with("RTSP/1.0 200"), "rtsp video only setup");
        const auto session = rtsp_header_value(setup, "Session:");
        require(!session.empty(), "rtsp video only session");
        const auto play = peer.request("PLAY " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 3\r\n"
                                       "Session: " +
                                       session + "\r\n\r\n");
        require(play.starts_with("RTSP/1.0 200"), "rtsp video only play");

        auto audio = make_audio_track();
        audio.clock_rate = 48'000;
        audio.codec_config = {0x11, 0x90};
        require(peer.update_track(std::move(audio)), "rtsp unsetup audio config update");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(peer.session_alive(), "rtsp unsetup audio change keeps video session");

        auto video = make_h265_track();
        video.codec_config = h265_config_updated;
        require(peer.update_track(std::move(video)), "rtsp setup h265 config update");
        for (int attempt = 0; attempt < 100 && peer.session_alive(); ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!peer.session_alive(), "rtsp setup video change closes video session");
    }

    {
        rtsp_output_test_peer peer;
        const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
        const auto describe = peer.request("DESCRIBE " + base +
                                           " RTSP/1.0\r\n"
                                           "CSeq: 1\r\n"
                                           "Accept: application/sdp\r\n\r\n");
        require(describe.starts_with("RTSP/1.0 200"), "rtsp audio only describe");

        const auto setup = peer.request("SETUP " + base +
                                        "/trackID=2 RTSP/1.0\r\n"
                                        "CSeq: 2\r\n"
                                        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
        require(setup.starts_with("RTSP/1.0 200"), "rtsp audio only setup");
        const auto session = rtsp_header_value(setup, "Session:");
        require(!session.empty(), "rtsp audio only session");
        const auto play = peer.request("PLAY " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 3\r\n"
                                       "Session: " +
                                       session + "\r\n\r\n");
        require(play.starts_with("RTSP/1.0 200"), "rtsp audio only play");

        auto video = make_video_track();
        video.codec_config = h264_config_updated;
        require(peer.update_track(std::move(video)), "rtsp unsetup video config update");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(peer.session_alive(), "rtsp unsetup video change keeps audio session");

        auto audio = make_audio_track();
        audio.clock_rate = 48'000;
        audio.codec_config = {0x11, 0x90};
        require(peer.update_track(std::move(audio)), "rtsp setup audio config update");
        for (int attempt = 0; attempt < 100 && peer.session_alive(); ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!peer.session_alive(), "rtsp setup audio change closes audio session");
    }
}

void test_rtsp_output_rejects_stale_description()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp stale describe");

    auto updated = make_video_track();
    updated.codec_config.push_back(0x01);
    require(peer.update_track(std::move(updated)), "rtsp stale source config update");

    const auto setup = peer.request("SETUP " + base +
                                    "/trackID=1 RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(setup.starts_with("RTSP/1.0 455"), "rtsp reject stale described config");
}

int capture_rtp_timestamp(void* param, int, const void*, int bytes, std::uint32_t timestamp, int)
{
    if (bytes > 0)
    {
        static_cast<rtp_timeline_capture*>(param)->timestamps.push_back(timestamp);
    }
    return 0;
}

void test_timebase_conversions()
{
    constexpr std::int64_t thirty_hours_ns = 30LL * 60 * 60 * 1'000'000'000;
    require(ns_to_90khz(thirty_hours_ns) == 9'720'000'000LL, "90khz long timeline");

    constexpr std::int64_t long_milliseconds = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1'234;
    const auto long_ns = milliseconds_to_ns(long_milliseconds);
    require(ns_to_milliseconds(long_ns) == long_milliseconds, "millisecond timeline keeps int64 range");
    require(ns_to_flv_milliseconds(long_ns) == 1'233U, "flv timestamp wraps uint32 timeline");

    constexpr std::int64_t negative_ns = -40'000'000;
    require(ns_to_milliseconds(negative_ns) == -40, "millisecond timeline keeps negative values");
    require(ns_to_flv_milliseconds(negative_ns) == std::numeric_limits<std::uint32_t>::max() - 39U,
            "flv timestamp keeps signed composition offset modulo uint32");
    require(ns_to_90khz(negative_ns) == -3'600, "90khz timeline keeps negative values");
}

void test_rtmp_timestamp_timeline()
{
    rtmp_timestamp_state state;
    const auto near_wrap = std::numeric_limits<std::uint32_t>::max() - 9U;
    require(unwrap_rtmp_timestamp(near_wrap, state) == static_cast<std::int64_t>(near_wrap), "rtmp timestamp initial value");
    require(unwrap_rtmp_timestamp(5U, state) == static_cast<std::int64_t>(near_wrap) + 15, "rtmp timestamp unwraps uint32 wrap");
    require(unwrap_rtmp_timestamp(45U, state) == static_cast<std::int64_t>(near_wrap) + 55, "rtmp timestamp continues after wrap");

    require(rtmp_timestamp_delta(960U, 1'000U) == -40, "rtmp signed negative composition offset");
    require(rtmp_timestamp_delta(1'040U, 1'000U) == 40, "rtmp signed positive composition offset");
}

void test_internal_format_contract()
{
    const auto avcc = h264_annex_b_to_avcc(h264_config);
    require(!avcc.empty(), "annex-b to avcc");
    const auto annex_b = h264_avcc_to_annex_b(avcc);
    require(!annex_b.empty(), "avcc to annex-b");
    require(annex_b.size() >= 8, "annex-b config size");
    require(annex_b[0] == 0 && annex_b[1] == 0 && annex_b[2] == 0 && annex_b[3] == 1, "annex-b four-byte start code");

    const auto hvcc = h265_annex_b_to_hvcc(h265_config);
    require(!hvcc.empty(), "h265 annex-b to hvcc");
    const auto hevc_annex_b = h265_hvcc_to_annex_b(hvcc);
    require(!hevc_annex_b.empty(), "h265 hvcc to annex-b");
    require(hevc_annex_b.size() >= 8U, "h265 annex-b config size");
    require(hevc_annex_b[0] == 0 && hevc_annex_b[1] == 0 && hevc_annex_b[2] == 0 && hevc_annex_b[3] == 1, "h265 annex-b four-byte start code");

    const std::vector<std::uint8_t> raw{0x11, 0x22, 0x33, 0x44};
    const auto adts = make_adts_frame(aac_asc, raw);
    require(adts.size() > raw.size(), "adts header exists");
    const auto aac = parse_aac_adts(adts);
    require(aac.has_value(), "parse adts");
    require(aac->sample_rate == 44'100 && aac->channel_count == 2, "adts aac config");
}

void test_rtmp_aac_asc_adts_contract()
{
    const std::vector<std::uint8_t> aac_ltp_asc{0x22, 0x10};
    const std::vector<std::uint8_t> he_aac_asc{0x2b, 0x92, 0x08, 0x00};
    const std::vector<std::uint8_t> reserved_object_type{0x02, 0x10};
    const std::vector<std::uint8_t> scalable_object_type{0x32, 0x10};
    const std::vector<std::uint8_t> raw_aac{0x11, 0x22, 0x33, 0x44};

    require(accepts_flv_aac_config(aac_asc), "rtmp aac lc config accepted");
    require(accepts_flv_aac_config(aac_ltp_asc), "rtmp highest adts aac object type accepted");
    require(accepts_flv_aac_config(he_aac_asc), "rtmp he-aac core config accepted");
    require(!accepts_flv_aac_config(reserved_object_type), "rtmp reserved aac object type rejected");
    require(!accepts_flv_aac_config(scalable_object_type), "rtmp non-adts aac object type rejected");
    require(make_adts_frame(reserved_object_type, raw_aac).empty(), "invalid aac object type cannot create adts");
    require(make_adts_frame(scalable_object_type, raw_aac).empty(), "unsupported aac object type cannot create adts");
}

void test_flv_config_cache_lifecycle()
{
    flv_demux_capture capture;
    const auto demuxer =
        std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    require(demuxer != nullptr, "flv config demuxer create");
    std::size_t video_sequence_headers = 0;
    std::size_t audio_sequence_headers = 0;
    std::optional<std::int32_t> video_composition_time;
    std::optional<std::uint32_t> video_timestamp;
    flv_output_muxer output(
        [&capture, &demuxer, &video_sequence_headers, &audio_sequence_headers, &video_composition_time, &video_timestamp](
            int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv config demuxer input");
            if (data.size() < 2U)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO && data[1] == 1U && data.size() >= 5U)
            {
                const auto raw =
                    (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 8U) | static_cast<std::uint32_t>(data[4]);
                video_composition_time =
                    raw < 0x00800000U ? static_cast<std::int32_t>(raw) : static_cast<std::int32_t>(static_cast<std::int64_t>(raw) - 0x01000000LL);
                video_timestamp = timestamp;
                return;
            }
            if (data[1] != 0U)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                ++video_sequence_headers;
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                ++audio_sequence_headers;
            }
        });

    auto video = make_video_track();
    video.config_version = 1;
    output.on_track(video);
    require(video_sequence_headers == 1U, "flv initial video sequence header");
    auto reordered_video = make_video_frame(-40'000'000, true);
    reordered_video.dts_ns = 0;
    output.on_frame(reordered_video);
    require(video_timestamp == 0U, "flv video dts stays on unsigned tag timeline");
    require(video_composition_time == -40, "flv video keeps negative composition time");

    auto audio = make_audio_track();
    audio.config_version = 1;
    output.on_track(audio);
    output.on_frame(make_audio_frame(0));
    require(audio_sequence_headers == 1U, "flv initial audio sequence header");

    const std::vector<std::uint8_t> updated_asc{0x11, 0x90};
    auto updated_audio = audio;
    updated_audio.clock_rate = 48'000;
    updated_audio.codec_config = updated_asc;
    updated_audio.config_version = 2;
    output.on_track(updated_audio);
    require(video_sequence_headers == 2U, "flv config generation resets cached video header");

    const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
    auto updated_adts = make_adts_frame(updated_asc, raw);
    require(!updated_adts.empty(), "flv updated aac adts");
    output.on_frame(media_frame{
        .track = audio_track_id,
        .dts_ns = 1'000'000'000,
        .pts_ns = 1'000'000'000,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(updated_adts)),
    });
    require(audio_sequence_headers == 2U, "flv config generation resets cached audio header");

    auto updated_video = video;
    updated_video.codec_config = h264_config_updated;
    updated_video.config_version = 2;
    output.on_track(updated_video);
    const auto avcc_count = std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    require(avcc_count == 3, "flv h264 config generations");
    const auto first_avcc = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    const auto last_avcc = std::ranges::find_if(
        capture.packets.rbegin(), capture.packets.rend(), [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    require(first_avcc != capture.packets.end() && h264_avcc_to_annex_b(first_avcc->payload) == h264_config, "flv initial h264 config content");
    require(last_avcc != capture.packets.rend() && h264_avcc_to_annex_b(last_avcc->payload) == h264_config_updated,
            "flv updated h264 config content");
}

void test_h265_output_paths()
{
    flv_demux_capture capture;
    const auto demuxer =
        std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    require(demuxer != nullptr, "flv h265 demuxer create");
    flv_output_muxer flv([&demuxer](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
                         { require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv h265 demuxer input"); });
    auto hevc_track = make_h265_track();
    hevc_track.config_version = 1;
    flv.on_track(hevc_track);
    const auto hevc_frame = make_h265_frame(40'000'000, true);
    flv.on_frame(hevc_frame);
    auto updated_hevc = hevc_track;
    updated_hevc.codec_config = h265_config_updated;
    updated_hevc.config_version = 2;
    flv.on_track(updated_hevc);

    require(std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_HVCC; }) == 2,
            "flv h265 config generations");
    const auto first_hvcc = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_HVCC; });
    const auto last_hvcc = std::ranges::find_if(
        capture.packets.rbegin(), capture.packets.rend(), [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_HVCC; });
    require(first_hvcc != capture.packets.end() && h265_hvcc_to_annex_b(first_hvcc->payload) == h265_config, "flv initial h265 config content");
    require(last_hvcc != capture.packets.rend() && h265_hvcc_to_annex_b(last_hvcc->payload) == h265_config_updated,
            "flv updated h265 config content");
    const auto media = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_H265; });
    require(media != capture.packets.end() && media->payload == *hevc_frame.payload, "flv h265 media payload");
    require(media->pts == 40 && media->dts == 40 && media->flags == 1, "flv h265 media timing and key frame");

    hls_output hls(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    hls.on_track(make_h265_track());
    hls.on_frame(make_h265_frame(0, true));
    hls.on_frame(make_h265_frame(1'000'000'000, true));
    hls.on_end();
    require(hls.segment_count() >= 1U, "hls h265 segment");
    const auto segment = hls.segment(0);
    require(segment.has_value(), "hls h265 ts");
    const auto ts_capture = demux_ts_segment(*segment);
    require(ts_capture.stream_codecs == std::vector<int>{PSI_STREAM_H265}, "hls h265 pmt stream type");
    require(ts_capture.packets.size() == 1U, "hls h265 packet count");
    require(ts_capture.packets.front().codec == PSI_STREAM_H265 && (ts_capture.packets.front().flags & MPEG_FLAG_IDR_FRAME) != 0,
            "hls h265 packet codec and key frame");
    require(ts_capture.packets.front().pts == 0 && ts_capture.packets.front().dts == 0, "hls h265 packet timestamp");
    require(ts_capture.packets.front().payload == *make_h265_frame(0, true).payload, "hls h265 packet payload");

    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output webrtc(
        webrtc_output_config{
            .video_codec = codec_id::h265,
            .video_payload_type = 103,
            .rtcp_cname = {},
        },
        [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
    webrtc.on_track(make_h265_track());
    require(webrtc.valid(), "webrtc h265 output valid");
    webrtc.on_frame(make_h265_frame(0, true));
    require(!packets.empty() && packets.front().size() >= 12U, "webrtc h265 rtp packet");
    require((packets.front()[1] & 0x7fU) == 103U, "webrtc h265 payload type");
}

void test_rtsp_muxer_zero_origin_timeline()
{
    rtp_timeline_capture capture;
    auto* muxer = rtsp_muxer_create(&capture_rtp_timestamp, &capture);
    require(muxer != nullptr, "rtsp muxer create");

    const auto payload = rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, 96, "H264", 0, 0x12345678U, 0, nullptr, 0);
    require(payload >= 0, "rtsp muxer add payload");
    const auto media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_H264, nullptr, 0);
    require(media >= 0, "rtsp muxer add media");

    const std::array<std::uint8_t, 8> frame{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
    require(rtsp_muxer_input(muxer, media, 0, 0, frame.data(), static_cast<int>(frame.size()), 0) == 0, "rtsp muxer first frame");
    require(rtsp_muxer_input(muxer, media, 40, 40, frame.data(), static_cast<int>(frame.size()), 0) == 0, "rtsp muxer second frame");
    require(capture.timestamps.size() == 2U, "rtsp muxer packet count");
    require(capture.timestamps[1] - capture.timestamps[0] == 3'600U, "rtsp muxer zero origin timestamp step");

    rtsp_muxer_destroy(muxer);
}

void test_rtsp_aac_adts_round_trip()
{
    rtsp_aac_capture capture;
    require(avpkt2bs_create(&capture.bitstream) == 0, "rtsp aac bitstream create");
    capture.demuxer = rtsp_demuxer_create(0, 0, &capture_rtsp_aac_frame, &capture);
    require(capture.demuxer != nullptr, "rtsp aac demuxer create");
    constexpr int payload_type = 96;
    constexpr std::string_view fmtp = "96 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210";
    require(rtsp_demuxer_add_payload(capture.demuxer, 44'100, payload_type, "MPEG4-GENERIC", fmtp.data()) == 0, "rtsp aac demuxer payload");

    auto* muxer = rtsp_muxer_create(&forward_rtsp_aac_rtp, &capture);
    require(muxer != nullptr, "rtsp aac muxer create");
    const auto payload = rtsp_muxer_add_payload(
        muxer, "RTP/AVP", 44'100, payload_type, "MPEG4-GENERIC", 0, 0x12345678U, 0, aac_asc.data(), static_cast<int>(aac_asc.size()));
    require(payload >= 0, "rtsp aac muxer payload");
    const auto media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_MP4A, aac_asc.data(), static_cast<int>(aac_asc.size()));
    require(media >= 0, "rtsp aac muxer media");

    const auto frame = make_audio_frame(20'000'000);
    require(rtsp_muxer_input(muxer, media, 20, 20, frame.payload->data(), static_cast<int>(frame.payload->size()), 0) == 0, "rtsp aac muxer input");
    require(capture.rtp_payload.size() >= 4U && capture.rtp_payload[0] == 0U && capture.rtp_payload[1] == 16U, "rtsp aac rtp au header");
    const auto au_size = (static_cast<std::size_t>(capture.rtp_payload[2]) << 5U) | (capture.rtp_payload[3] >> 3U);
    const auto raw_aac = std::span<const std::uint8_t>(*frame.payload).subspan(7U);
    require(au_size == raw_aac.size() && std::ranges::equal(std::span<const std::uint8_t>(capture.rtp_payload).subspan(4U), raw_aac),
            "rtsp aac rtp carries raw access unit");
    require(capture.frame == *frame.payload, "rtsp aac round trip keeps one adts header");
    require(rtsp_muxer_destroy(muxer) == 0, "rtsp aac muxer destroy");
    require(rtsp_demuxer_destroy(capture.demuxer) == 0, "rtsp aac demuxer destroy");
    require(avpkt2bs_destroy(&capture.bitstream) == 0, "rtsp aac bitstream destroy");
}

void test_media_stream_fanout_and_reentrancy()
{
    media_stream stream("live/test");
    require(stream.update_track(make_video_track()), "add video track");

    auto first = std::make_shared<counting_sink>();
    auto removing = std::make_shared<self_removing_sink>(stream);
    auto second = std::make_shared<counting_sink>();

    stream.add_sink(first);
    stream.add_sink(removing);
    stream.add_sink(second);
    require(first->tracks == 1 && second->tracks == 1, "late sink track snapshot");

    stream.publish(make_video_frame(0, true));
    require(removing->frames == 1, "self-removing sink receives first frame");
    require(first->frames == 1 && second->frames == 1, "other sinks survive reentrant remove");

    stream.publish(make_video_frame(40'000'000, false));
    require(removing->frames == 1, "removed sink not called again");
    require(first->frames == 2 && second->frames == 2, "remaining sinks continue");

    stream.end();
    require(first->ends == 1 && second->ends == 1, "stream end fanout");

    media_stream track_remove_stream("live/track-remove");
    require(track_remove_stream.update_track(make_video_track()), "track remove video");
    require(track_remove_stream.update_track(make_audio_track()), "track remove audio");
    auto track_removing = std::make_shared<track_removing_sink>(track_remove_stream);
    track_remove_stream.add_sink(track_removing);
    require(track_removing->tracks == 1, "removed sink stops track replay");
    track_remove_stream.publish(make_video_frame(0, true));
    require(track_removing->frames == 0, "track removed sink receives no frame");

    media_stream track_end_stream("live/track-end");
    require(track_end_stream.update_track(make_video_track()), "track end video");
    require(track_end_stream.update_track(make_audio_track()), "track end audio");
    auto track_ending = std::make_shared<track_ending_sink>(track_end_stream);
    track_end_stream.add_sink(track_ending);
    require(track_end_stream.ended(), "track callback stream ended");
    require(track_ending->tracks == 1, "ended stream stops track replay");
    require(track_ending->ends == 1, "new sink receives end during track replay");

    media_stream update_remove_stream("live/update-remove");
    auto update_removed = std::make_shared<counting_sink>();
    auto update_removing = std::make_shared<track_other_removing_sink>(update_remove_stream, *update_removed);
    update_remove_stream.add_sink(update_removing);
    update_remove_stream.add_sink(update_removed);
    require(update_remove_stream.update_track(make_video_track()), "update removes later sink");
    require(update_removing->tracks == 1, "update removing sink receives track");
    require(update_removed->tracks == 0, "update removed sink skips current track");

    media_stream publish_remove_stream("live/publish-remove");
    require(publish_remove_stream.update_track(make_video_track()), "publish remove video");
    auto publish_removed = std::make_shared<counting_sink>();
    auto publish_removing = std::make_shared<frame_other_removing_sink>(publish_remove_stream, *publish_removed);
    publish_remove_stream.add_sink(publish_removing);
    publish_remove_stream.add_sink(publish_removed);
    publish_remove_stream.publish(make_video_frame(0, true));
    require(publish_removing->frames == 1, "publish removing sink receives frame");
    require(publish_removed->frames == 0, "publish removed sink skips current frame");

    media_stream update_end_stream("live/update-end");
    auto update_ending = std::make_shared<track_ending_sink>(update_end_stream);
    auto update_after = std::make_shared<counting_sink>();
    update_end_stream.add_sink(update_ending);
    update_end_stream.add_sink(update_after);
    require(update_end_stream.update_track(make_video_track()), "update callback ends stream");
    require(update_end_stream.ended(), "update callback stream ended");
    require(update_ending->tracks == 1 && update_ending->ends == 1, "update ending sink lifecycle");
    require(update_after->tracks == 0 && update_after->ends == 1, "update stops callbacks after end");

    media_stream publish_end_stream("live/publish-end");
    require(publish_end_stream.update_track(make_video_track()), "publish end video");
    auto frame_ending = std::make_shared<frame_ending_sink>(publish_end_stream);
    auto frame_after = std::make_shared<counting_sink>();
    publish_end_stream.add_sink(frame_ending);
    publish_end_stream.add_sink(frame_after);
    publish_end_stream.publish(make_video_frame(0, true));
    require(publish_end_stream.ended(), "frame callback stream ended");
    require(frame_ending->frames == 1 && frame_ending->ends == 1, "frame ending sink lifecycle");
    require(frame_after->frames == 0 && frame_after->ends == 1, "publish stops callbacks after end");

    media_stream version_stream("live/version");
    auto first_version = make_video_track();
    first_version.config_version = 99;
    require(version_stream.update_track(std::move(first_version)), "version first track");
    require(version_stream.tracks().front().config_version == 1, "stream owns initial config version");
    auto version_observer = std::make_shared<track_version_sink>();
    version_stream.add_sink(version_observer);
    require(!version_stream.update_track(make_video_track()), "identical config ignored");
    require(version_observer->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}}, "identical config not fanned out");
    auto changed_kind = make_video_track(2);
    changed_kind.kind = media_kind::audio;
    require(!version_stream.update_track(std::move(changed_kind)), "track kind change rejected");
    auto changed_codec = make_video_track(2);
    changed_codec.codec = codec_id::aac;
    require(!version_stream.update_track(std::move(changed_codec)), "track codec change rejected");
    auto second_version = make_video_track(2);
    second_version.config_version = 99;
    require(version_stream.update_track(std::move(second_version)), "changed config accepted");
    require(version_stream.tracks().front().config_version == 2, "stream increments config version");
    require(!version_stream.update_track(make_video_track(2)), "changed config duplicate ignored");

    media_stream update_reentrant_stream("live/update-reentrant");
    require(update_reentrant_stream.update_track(make_video_track()), "reentrant first track");
    auto update_reentrant = std::make_shared<track_updating_sink>(update_reentrant_stream, video_track_id, 2, make_video_track(3));
    auto update_observer = std::make_shared<track_version_sink>();
    update_reentrant_stream.add_sink(update_reentrant);
    update_reentrant_stream.add_sink(update_observer);
    require(update_reentrant_stream.update_track(make_video_track(2)), "reentrant outer update");
    require(update_reentrant->update_succeeded, "reentrant nested update");
    require(update_observer->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {video_track_id, 3}},
            "reentrant stale update skipped");

    media_stream replay_reentrant_stream("live/replay-reentrant");
    require(replay_reentrant_stream.update_track(make_video_track()), "replay video track");
    require(replay_reentrant_stream.update_track(make_audio_track()), "replay audio track");
    auto replay_reentrant = std::make_shared<track_updating_sink>(replay_reentrant_stream, video_track_id, 1, make_audio_track(2));
    replay_reentrant_stream.add_sink(replay_reentrant);
    require(replay_reentrant->update_succeeded, "replay nested update");
    require(replay_reentrant->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {audio_track_id, 2}},
            "replay stale track skipped");
}

void test_media_stream_gop_cache()
{
    media_stream stream("live/gop");
    require(stream.update_track(make_video_track()), "gop video track");
    require(stream.update_track(make_audio_track()), "gop audio track");

    stream.publish(make_audio_frame(-20'000'000));
    stream.publish(make_video_frame(0, true));
    stream.publish(make_audio_frame(20'000'000));
    stream.publish(make_video_frame(40'000'000, false));

    auto first = std::make_shared<counting_sink>();
    stream.add_sink(first);
    require(first->tracks == 2, "gop replays track configuration first");
    require(first->received_frames ==
                std::vector<std::pair<track_id, std::int64_t>>{
                    {video_track_id, 0},
                    {audio_track_id, 20'000'000},
                    {video_track_id, 40'000'000},
                },
            "gop replays frames from latest key frame");

    stream.publish(make_video_frame(1'000'000'000, true));
    stream.publish(make_audio_frame(1'020'000'000));
    stream.publish(make_video_frame(1'040'000'000, false));

    auto second = std::make_shared<counting_sink>();
    stream.add_sink(second);
    require(second->received_frames ==
                std::vector<std::pair<track_id, std::int64_t>>{
                    {video_track_id, 1'000'000'000},
                    {audio_track_id, 1'020'000'000},
                    {video_track_id, 1'040'000'000},
                },
            "gop keeps only latest gop");

    auto removing = std::make_shared<self_removing_sink>(stream);
    stream.add_sink(removing);
    require(removing->frames == 1, "gop replay stops after sink removal");

    require(stream.update_track(make_audio_track(2)), "gop audio config change");
    auto after_config = std::make_shared<counting_sink>();
    stream.add_sink(after_config);
    require(after_config->frames == 0, "gop cache cleared on track config change");

    media_stream overflow_stream("live/gop-overflow");
    require(overflow_stream.update_track(make_video_track()), "gop overflow track");
    overflow_stream.publish(make_video_frame(0, true));
    for (std::int64_t index = 1; index <= 2500; ++index)
    {
        overflow_stream.publish(make_video_frame(index, false));
    }
    auto overflow_sink = std::make_shared<counting_sink>();
    overflow_stream.add_sink(overflow_sink);
    require(overflow_sink->frames == 0, "gop overflow drops incomplete cache");

    media_stream h265_stream("live/gop-h265");
    require(h265_stream.update_track(make_h265_track()), "gop h265 track");
    h265_stream.publish(make_h265_frame(0, true));
    h265_stream.publish(make_h265_frame(40'000'000, false));
    auto h265_sink = std::make_shared<counting_sink>();
    h265_stream.add_sink(h265_sink);
    require(h265_sink->received_frames ==
                std::vector<std::pair<track_id, std::int64_t>>{{video_track_id, 0}, {video_track_id, 40'000'000}},
            "gop h265 replay");
}

void test_media_stream_worker_fanout()
{
    io_context_pool workers(2);
    auto stream = std::make_shared<media_stream>("live/threaded", workers.context(0).get_executor());

    std::atomic_int ended_count{};
    auto first = std::make_shared<worker_sink>(ended_count, workers);
    auto second = std::make_shared<worker_sink>(ended_count, workers);
    auto third = std::make_shared<worker_sink>(ended_count, workers);

    boost::asio::post(workers.context(0),
                      [&, stream]()
                      {
                          require(stream->update_track(make_video_track()), "threaded stream track");
                          stream->add_sink(first, workers.context(0).get_executor());
                          stream->add_sink(second, workers.context(0).get_executor());
                          stream->add_sink(third, workers.context(1).get_executor());

                          auto first_frame = make_video_frame(0, true);
                          const auto first_payload = first_frame.payload.get();
                          stream->publish(std::move(first_frame));
                          auto second_frame = make_video_frame(40'000'000, false);
                          const auto second_payload = second_frame.payload.get();
                          stream->publish(std::move(second_frame));
                          stream->end();

                          require(first_payload != nullptr && second_payload != nullptr, "threaded payload identity source");
                      });
    workers.run();

    require(first->tracks() == 1 && second->tracks() == 1 && third->tracks() == 1, "threaded track replay");
    require(first->frames() == std::vector<std::int64_t>{0, 40'000'000}, "threaded first frame order");
    require(second->frames() == std::vector<std::int64_t>{0, 40'000'000}, "threaded second frame order");
    require(third->frames() == std::vector<std::int64_t>{0, 40'000'000}, "threaded third frame order");
    require(first->ends() == 1 && second->ends() == 1 && third->ends() == 1, "threaded stream end");
    require(first->thread() == second->thread(), "same worker sinks share thread");
    require(first->thread() != third->thread(), "different worker sinks use different threads");

    const auto first_payloads = first->payloads();
    const auto second_payloads = second->payloads();
    const auto third_payloads = third->payloads();
    require(first_payloads.size() == 2U && second_payloads == first_payloads && third_payloads == first_payloads,
            "threaded fanout shares frame payload");
}

void test_media_stream_worker_removal()
{
    io_context_pool workers(2);
    auto stream = std::make_shared<media_stream>("live/threaded-removal", workers.context(0).get_executor());

    auto removed = std::make_shared<counting_sink>();
    auto removing = std::make_shared<frame_other_removing_sink>(*stream, *removed);
    boost::asio::post(workers.context(0),
                      [&, stream]()
                      {
                          require(stream->update_track(make_video_track()), "threaded removal track");
                          const auto executor = workers.context(1).get_executor();
                          stream->add_sink(removing, executor);
                          stream->add_sink(removed, executor);
                          stream->publish(make_video_frame(0, true));
                          boost::asio::post(workers.context(1), [&workers]() { workers.release_work(); });
                      });
    workers.run();

    require(removing->frames == 1, "threaded removing sink receives frame");
    require(removed->frames == 0, "threaded removal suppresses pending callback");
}

void test_media_stream_pull_reader_overrun()
{
    io_context_pool workers(2);
    auto stream = std::make_shared<media_stream>("live/pull-overrun", workers.context(0).get_executor());
    auto fast = std::make_shared<pull_test_reader>(true);
    auto stalled = std::make_shared<pull_test_reader>(false);
    std::thread::id owner_thread;

    boost::asio::post(workers.context(0),
                      [&, stream]()
                      {
                          owner_thread = std::this_thread::get_id();
                          require(stream->update_track(make_video_track()), "pull overrun track");
                          static_cast<void>(stream->add_reader(fast, workers.context(1).get_executor()));
                          static_cast<void>(stream->add_reader(stalled, workers.context(1).get_executor()));
                      });

    std::thread runner([&workers]() { workers.run(); });
    require(fast->wait_for_ready(1) && stalled->wait_for_ready(1), "pull readers ready");

    boost::asio::post(workers.context(0), [stream]() { stream->publish(make_video_frame(0, true)); });
    require(fast->wait_for_frames(1) && stalled->wait_for_frames(1), "pull readers receive first key frame");

    const std::array<std::pair<std::int64_t, bool>, 4> frames{
        std::pair{40'000'000LL, false},
        std::pair{1'000'000'000LL, true},
        std::pair{1'040'000'000LL, false},
        std::pair{2'000'000'000LL, true},
    };
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const auto [pts, key_frame] = frames[index];
        boost::asio::post(workers.context(0), [stream, pts, key_frame]() { stream->publish(make_video_frame(pts, key_frame)); });
        require(fast->wait_for_frames(index + 2), "fast pull reader keeps pace");
    }

    stalled->request();
    require(stalled->wait_for_frames(2), "stalled pull reader resynchronizes");

    boost::asio::post(workers.context(0), [stream]() { stream->end(); });
    require(fast->wait_for_ends(1) && stalled->wait_for_ends(1), "pull readers receive end");
    workers.release_work();
    runner.join();

    require(fast->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{
                    {1, 0},
                    {1, 40'000'000},
                    {1, 1'000'000'000},
                    {1, 1'040'000'000},
                    {1, 2'000'000'000},
                },
            "fast pull reader receives every frame");
    require(stalled->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{1, 0}, {1, 2'000'000'000}},
            "stalled pull reader resumes at latest key frame");
    require(fast->thread() == stalled->thread() && fast->thread() != owner_thread, "pull reader callbacks stay on reader worker");
}

void test_media_stream_pull_reader_duplicate_read()
{
    boost::asio::io_context owner(1);
    boost::asio::io_context reader_worker(1);
    const auto drain = [](boost::asio::io_context& io)
    {
        io.restart();
        while (io.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-duplicate", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(false, false);
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          require(stream->update_track(make_video_track()), "pull duplicate track");
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);

    reader->request();
    reader->request();
    reader->request();
    drain(owner);
    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_video_frame(40'000'000, false));
                          stream->publish(make_video_frame(80'000'000, false));
                      });
    drain(owner);

    reader->request();
    reader->request();
    reader->request();
    drain(owner);
    require(reader->frames().empty(), "posted pull callback waits for reader executor");
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{1, 0}},
            "duplicate pull requests produce one callback");

    reader->request();
    drain(owner);
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{1, 0}, {1, 40'000'000}},
            "next pull starts after callback begins");
}

void test_media_stream_pull_reader_previous_gop_continuity()
{
    boost::asio::io_context owner(1);
    boost::asio::io_context reader_worker(1);
    const auto drain = [](boost::asio::io_context& io)
    {
        io.restart();
        while (io.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-continuity", owner.get_executor());
    auto continuity_reader = std::make_shared<pull_test_reader>(false, false);
    auto overrun_reader = std::make_shared<pull_test_reader>(false, false);
    boost::asio::post(owner,
                      [stream, continuity_reader, overrun_reader, &reader_worker]()
                      {
                          require(stream->update_track(make_video_track()), "pull continuity track");
                          static_cast<void>(stream->add_reader(continuity_reader, reader_worker.get_executor()));
                          static_cast<void>(stream->add_reader(overrun_reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_video_frame(40'000'000, false));
                          stream->publish(make_video_frame(80'000'000, false));
                      });
    drain(owner);
    continuity_reader->request();
    overrun_reader->request();
    drain(owner);
    drain(reader_worker);

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(1'000'000'000, true));
                          stream->publish(make_video_frame(1'040'000'000, false));
                      });
    drain(owner);
    for (std::size_t index = 0; index < 4; ++index)
    {
        continuity_reader->request();
        drain(owner);
        drain(reader_worker);
    }
    require(continuity_reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{
                    {1, 0},
                    {1, 40'000'000},
                    {1, 80'000'000},
                    {1, 1'000'000'000},
                    {1, 1'040'000'000},
                },
            "reader continues through previous gop");

    boost::asio::post(owner, [stream]() { stream->publish(make_video_frame(2'000'000'000, true)); });
    drain(owner);
    overrun_reader->request();
    drain(owner);
    drain(reader_worker);
    require(overrun_reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{1, 0}, {1, 2'000'000'000}},
            "overrun reader resumes at current gop key frame");
}

void test_media_stream_add_reader_after_end()
{
    boost::asio::io_context owner(1);
    boost::asio::io_context reader_worker(1);
    const auto drain = [](boost::asio::io_context& io)
    {
        io.restart();
        while (io.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-ended", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(false, false);
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          stream->end();
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);

    require(reader->track_versions().empty(), "ended stream reader receives no tracks");
    require(reader->ready_generations().empty(), "ended stream reader is never ready");
    require(reader->frames().empty(), "ended stream reader receives no frames");
    require(reader->ends() == 1, "ended stream reader receives one terminal event");
}

void test_media_stream_pull_reader_generation_order()
{
    boost::asio::io_context owner(1);
    boost::asio::io_context reader_worker(1);
    const auto drain = [](boost::asio::io_context& io)
    {
        io.restart();
        while (io.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-generation", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(true);
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          require(stream->update_track(make_video_track()), "pull generation initial track");
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);
    drain(owner);

    boost::asio::post(owner, [stream]() { stream->publish(make_video_frame(0, true)); });
    drain(owner);
    boost::asio::post(owner, [stream]() { require(stream->update_track(make_video_track(2)), "pull generation track reset"); });
    drain(owner);
    drain(reader_worker);

    require(reader->frames().empty(), "track reset suppresses old posted frame");
    require(reader->track_versions() ==
                std::vector<std::pair<media_reader_generation, std::uint64_t>>{{1, 1}, {2, 2}},
            "track reset starts a new ordered generation");
    require(reader->ready_generations() == std::vector<media_reader_generation>{1, 2}, "track reset publishes ready per generation");

    drain(owner);
    boost::asio::post(owner, [stream]() { stream->publish(make_video_frame(1'000'000'000, true)); });
    drain(owner);
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{2, 1'000'000'000}},
            "new generation receives its key frame");

    drain(owner);
    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(1'040'000'000, false));
                          stream->end();
                      });
    drain(owner);
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<media_reader_generation, std::int64_t>>{{2, 1'000'000'000}},
            "end suppresses old posted frame");
    require(reader->ends() == 1 && reader->end_generation() == 3, "end is terminal generation event");

    auto remove_stream = std::make_shared<media_stream>("live/pull-remove", owner.get_executor());
    auto removed_reader = std::make_shared<pull_test_reader>(true);
    boost::asio::post(owner,
                      [remove_stream, removed_reader, &reader_worker]()
                      {
                          require(remove_stream->update_track(make_video_track()), "pull remove track");
                          static_cast<void>(remove_stream->add_reader(removed_reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);
    drain(owner);
    boost::asio::post(owner, [remove_stream]() { remove_stream->publish(make_video_frame(0, true)); });
    drain(owner);
    removed_reader->remove();
    drain(reader_worker);
    drain(owner);
    require(removed_reader->frames().empty() && removed_reader->ends() == 0, "remove suppresses already posted callbacks");
}

void test_stream_registry_generation_lifecycle()
{
    stream_registry registry;

    auto first = std::make_shared<media_stream>("live/generation");
    auto second = std::make_shared<media_stream>("live/generation");
    require(registry.add(first), "registry first generation add");
    require(!registry.add(second), "registry active generation duplicate reject");
    require(registry.find("live/generation").get() == first.get(), "registry first generation remains");

    auto replacing = std::make_shared<generation_replacing_sink>(registry, second);
    first->add_sink(replacing);
    first->end();
    require(replacing->old_generation_hidden, "registry ended generation hidden during end callback");
    require(replacing->replacement_added, "registry ended generation replace during end callback");
    require(registry.find("live/generation").get() == second.get(), "registry replacement generation visible");
    registry.remove(*first);
    require(registry.find("live/generation").get() == second.get(), "registry stale remove preserves replacement");

    auto ended = std::make_shared<media_stream>("live/ended");
    ended->end();
    require(!registry.add(ended), "registry ended generation add reject");

    registry.remove(*second);
    require(!registry.find("live/generation"), "registry replacement removed");
}

void test_hls_output()
{
    hls_output output(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    output.on_track(make_video_track());
    output.on_track(make_audio_track());

    // 开始前的音频必须等待自然视频关键帧。
    output.on_frame(make_audio_frame(0));
    output.on_frame(make_video_frame(0, true));
    output.on_frame(make_audio_frame(20'000'000));
    output.on_frame(make_video_frame(500'000'000, false));
    output.on_frame(make_audio_frame(520'000'000));
    output.on_frame(make_video_frame(1'000'000'000, true));
    output.on_frame(make_audio_frame(1'020'000'000));
    output.on_frame(make_video_frame(1'500'000'000, false));
    output.on_frame(make_video_frame(2'000'000'000, true));
    output.on_end();

    require(output.segment_count() >= 2, "hls segment count");
    const auto first = output.segment(0);
    require(first.has_value() && !first->empty(), "first hls segment");
    const auto first_capture = demux_ts_segment(*first);
    require(first_capture.stream_codecs == std::vector<int>{PSI_STREAM_AAC, PSI_STREAM_H264}, "hls audio video pmt stream types");
    require(std::ranges::count_if(first_capture.packets, [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_H264; }) == 2,
            "hls h264 packet count");
    require(std::ranges::count_if(first_capture.packets, [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_AAC; }) == 2,
            "hls aac packet count");
    const auto video_key =
        std::ranges::find_if(first_capture.packets, [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_H264 && packet.pts == 0; });
    const auto video_delta = std::ranges::find_if(
        first_capture.packets, [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_H264 && packet.pts == 45'000; });
    const auto audio_first = std::ranges::find_if(first_capture.packets,
                                                  [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_AAC && packet.pts == 1'800; });
    const auto audio_second = std::ranges::find_if(
        first_capture.packets, [](const demuxed_packet& packet) { return packet.codec == PSI_STREAM_AAC && packet.pts == 46'800; });
    require(video_key != first_capture.packets.end() && (video_key->flags & MPEG_FLAG_IDR_FRAME) != 0 &&
                video_key->payload == *make_video_frame(0, true).payload,
            "hls h264 key frame payload");
    require(video_delta != first_capture.packets.end() && video_delta->payload == *make_video_frame(500'000'000, false).payload,
            "hls h264 delta frame payload");
    require(audio_first != first_capture.packets.end() && audio_first->payload == *make_audio_frame(20'000'000).payload,
            "hls first aac frame payload");
    require(audio_second != first_capture.packets.end() && audio_second->payload == *make_audio_frame(520'000'000).payload,
            "hls second aac frame payload");

    const auto playlist = output.playlist(".");
    require(playlist.find("#EXTM3U") != std::string::npos, "hls playlist header");
    require(playlist.find("#EXT-X-ENDLIST") != std::string::npos, "hls endlist");

    hls_output reconfigured(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    reconfigured.on_track(make_video_track());
    reconfigured.on_frame(make_video_frame(0, true));
    reconfigured.on_frame(make_video_frame(500'000'000, false));
    reconfigured.on_track(make_video_track(2));
    require(reconfigured.segment_count() == 1U, "hls config change closes current segment");
    reconfigured.on_frame(make_video_frame(600'000'000, false));
    require(reconfigured.segment_count() == 1U, "hls config change waits key frame");
    reconfigured.on_frame(make_video_frame(1'000'000'000, true));
    reconfigured.on_end();
    require(reconfigured.segment_count() == 2U, "hls config change starts new segment");

    hls_output audio_only(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    audio_only.on_track(make_audio_track());
    audio_only.on_frame(make_audio_frame(0));
    audio_only.on_frame(make_audio_frame(500'000'000));
    require(audio_only.segment_count() == 0U, "hls audio waits target duration");
    audio_only.on_frame(make_audio_frame(1'000'000'000));
    require(audio_only.segment_count() == 1U, "hls audio target duration segment");
    audio_only.on_end();
    require(audio_only.segment_count() == 2U, "hls audio final segment");
    const auto audio_segment = audio_only.segment(0);
    require(audio_segment.has_value(), "hls audio segment data");
    const auto audio_capture = demux_ts_segment(*audio_segment);
    require(audio_capture.stream_codecs == std::vector<int>{PSI_STREAM_AAC}, "hls audio pmt stream type");
    require(audio_capture.packets.size() == 2U, "hls audio packet count");
    require(audio_capture.packets[0].codec == PSI_STREAM_AAC && audio_capture.packets[0].pts == 0 &&
                audio_capture.packets[0].payload == *make_audio_frame(0).payload,
            "hls first audio-only packet");
    require(audio_capture.packets[1].codec == PSI_STREAM_AAC && audio_capture.packets[1].pts == 45'000 &&
                audio_capture.packets[1].payload == *make_audio_frame(500'000'000).payload,
            "hls second audio-only packet");

    hls_output signed_timeline(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    signed_timeline.on_track(make_audio_track());
    signed_timeline.on_frame(make_audio_frame(-500'000'000));
    signed_timeline.on_frame(make_audio_frame(500'000'000));
    require(signed_timeline.segment_count() == 1U, "hls signed pts reaches target duration");
    signed_timeline.on_end();

}

void test_hls_service_lifecycle()
{
    stream_registry registry;
    hls_service hls(registry, hls_config{.target_duration_seconds = 1.0, .window_size = 4});

    auto first = std::make_shared<media_stream>("live/hls");
    require(registry.add(first), "hls first stream add");
    require(first->update_track(make_video_track()), "hls first track");
    require(hls.segment_count("live/hls") == 0U, "hls first output create");

    first->publish(make_video_frame(0, true));
    first->publish(make_video_frame(1'000'000'000, true));
    first->end();
    registry.remove(*first);

    const auto ended_playlist = hls.playlist("live/hls");
    require(ended_playlist.has_value(), "hls ended playlist retained");
    require(ended_playlist->find("#EXT-X-ENDLIST") != std::string::npos, "hls ended playlist marker");
    const auto ended_segment = hls.segment("live/hls", 0);
    require(ended_segment.has_value() && !ended_segment->empty(), "hls ended segment retained");

    auto second = std::make_shared<media_stream>("live/hls");
    require(registry.add(second), "hls replacement stream add");
    require(second->update_track(make_video_track()), "hls replacement track");
    require(hls.segment_count("live/hls") == 0U, "hls replacement output reset");
    require(!hls.segment("live/hls", 0).has_value(), "hls replacement drops old segment");

    stream_registry expiring_registry;
    hls_service expiring_hls(expiring_registry, hls_config{.target_duration_seconds = 0.001, .window_size = 1});
    auto expiring_stream = std::make_shared<media_stream>("live/expiring");
    require(expiring_registry.add(expiring_stream), "hls expiring stream add");
    require(expiring_stream->update_track(make_video_track()), "hls expiring track");
    require(expiring_hls.segment_count("live/expiring") == 0U, "hls expiring output create");
    expiring_stream->end();
    expiring_registry.remove(*expiring_stream);
    require(expiring_hls.playlist("live/expiring").has_value(), "hls ended output initially retained");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!expiring_hls.playlist("live/expiring").has_value(), "hls ended output expires");
}

std::uint32_t rtp_timestamp(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp timestamp packet size");
    return (static_cast<std::uint32_t>(packet[4]) << 24U) | (static_cast<std::uint32_t>(packet[5]) << 16U) |
           (static_cast<std::uint32_t>(packet[6]) << 8U) | static_cast<std::uint32_t>(packet[7]);
}

std::uint16_t network_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 2U <= data.size(), "network u16 range");
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) | static_cast<std::uint16_t>(data[offset + 1U]));
}

std::uint32_t network_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 4U <= data.size(), "network u32 range");
    return (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) | static_cast<std::uint32_t>(data[offset + 3U]);
}

std::uint32_t rtp_ssrc(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp ssrc packet size");
    return network_u32(packet, 8U);
}

std::uint32_t require_rtcp_sender_report(const std::vector<std::uint8_t>& packet, std::string_view expected_cname)
{
    require(packet.size() >= 40U, "rtcp compound size");
    require((packet[0] >> 6U) == 2U, "rtcp sr version");
    require(packet[1] == 200U, "rtcp sender report type");

    const auto sr_size = (static_cast<std::size_t>(network_u16(packet, 2U)) + 1U) * 4U;
    require(sr_size >= 28U && sr_size + 12U <= packet.size(), "rtcp sender report size");
    const auto sender_ssrc = network_u32(packet, 4U);
    require(network_u32(packet, 20U) > 0U, "rtcp sender packet count");
    require(network_u32(packet, 24U) > 0U, "rtcp sender octet count");

    require((packet[sr_size] >> 6U) == 2U, "rtcp sdes version");
    require(packet[sr_size + 1U] == 202U, "rtcp sdes type");
    const auto sdes_size = (static_cast<std::size_t>(network_u16(packet, sr_size + 2U)) + 1U) * 4U;
    require(sr_size + sdes_size == packet.size(), "rtcp compound boundary");
    require(network_u32(packet, sr_size + 4U) == sender_ssrc, "rtcp sdes sender ssrc");
    require(packet[sr_size + 8U] == 1U, "rtcp sdes cname type");

    const auto cname_size = static_cast<std::size_t>(packet[sr_size + 9U]);
    require(sr_size + 10U + cname_size <= packet.size(), "rtcp cname range");
    const auto cname = std::string_view(reinterpret_cast<const char*>(packet.data() + sr_size + 10U), cname_size);
    require(cname == expected_cname, "rtcp sdes cname");
    return sender_ssrc;
}

void test_webrtc_rtp_packetizer()
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(webrtc_output_config{.video_payload_type = 102, .rtcp_cname = {}},
                         [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
    output.on_track(make_video_track());
    require(output.valid(), "webrtc video output valid");
    output.on_frame(make_video_frame(-40'000'000, false));
    require(packets.empty(), "webrtc waits natural key frame");
    output.on_frame(make_video_frame(0, true));

    require(!packets.empty() && packets.front().size() >= 12, "rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "rtp version 2");
    require((packets.front()[1] & 0x7fU) == 102U, "rtp negotiated payload type");

    const auto first_timestamp = rtp_timestamp(packets.front());
    const auto first_frame_packet_count = packets.size();
    output.on_frame(make_video_frame(40'000'000, false));
    require(packets.size() > first_frame_packet_count, "webrtc second h264 frame packetized");
    require(rtp_timestamp(packets.back()) - first_timestamp == 3'600U, "h264 rtp timestamp step");
}

void test_webrtc_opus_channel_count(int channel_count)
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{
            .opus_payload_type = 111,
            .opus_channel_count = channel_count,
            .rtcp_cname = {},
        },
        [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
    output.on_track(make_audio_track());
    require(output.valid(), "webrtc audio output valid");

    std::int64_t pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = pts_ns,
            .pts_ns = pts_ns,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        pts_ns += 23'219'954;
    }

    require(!packets.empty() && packets.front().size() >= 12, "opus rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "opus rtp version 2");
    require((packets.front()[1] & 0x7fU) == 111U, "opus negotiated payload type");

    if (packets.size() >= 2U)
    {
        require(rtp_timestamp(packets[1]) - rtp_timestamp(packets[0]) == 960U, "opus rtp timestamp step");
    }
}

void test_webrtc_output_initialization_failure()
{
    webrtc_output invalid_video(webrtc_output_config{.video_payload_type = 102, .rtcp_cname = {}}, [](std::span<const std::uint8_t>) {});
    auto video = make_video_track();
    video.codec_config.clear();
    invalid_video.on_track(video);
    require(!invalid_video.valid(), "webrtc invalid h264 output rejected");

    webrtc_output invalid_audio(webrtc_output_config{.opus_payload_type = 111, .opus_channel_count = 3, .rtcp_cname = {}},
                                [](std::span<const std::uint8_t>) {});
    invalid_audio.on_track(make_audio_track());
    require(!invalid_audio.valid(), "webrtc invalid opus output rejected");
}

void test_webrtc_rtcp_sender()
{
    constexpr std::string_view cname = "webrtc-test-cname";
    std::vector<std::vector<std::uint8_t>> rtp_packets;
    std::vector<std::vector<std::uint8_t>> rtcp_packets;
    webrtc_output output(
        webrtc_output_config{
            .video_payload_type = 102,
            .opus_payload_type = 111,
            .opus_channel_count = 2,
            .rtcp_cname = std::string(cname),
        },
        [&rtp_packets](std::span<const std::uint8_t> packet) { rtp_packets.emplace_back(packet.begin(), packet.end()); },
        [&rtcp_packets](std::span<const std::uint8_t> packet) { rtcp_packets.emplace_back(packet.begin(), packet.end()); });

    output.on_track(make_video_track());
    output.on_track(make_audio_track());
    output.on_frame(make_video_frame(0, true));

    std::int64_t audio_pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = audio_pts_ns,
            .pts_ns = audio_pts_ns,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        audio_pts_ns += 23'219'954;
    }

    std::optional<std::uint32_t> video_ssrc;
    std::optional<std::uint32_t> audio_ssrc;
    for (const auto& packet : rtp_packets)
    {
        require(packet.size() >= 12U, "rtcp sender rtp header");
        const auto payload_type = static_cast<std::uint8_t>(packet[1] & 0x7fU);
        if (payload_type == 102U)
        {
            video_ssrc = rtp_ssrc(packet);
        }
        else if (payload_type == 111U)
        {
            audio_ssrc = rtp_ssrc(packet);
        }
    }
    require(video_ssrc.has_value(), "rtcp sender video ssrc");
    require(audio_ssrc.has_value(), "rtcp sender audio ssrc");
    require(*video_ssrc != *audio_ssrc, "rtcp sender independent ssrc");

    bool video_report = false;
    bool audio_report = false;
    for (const auto& packet : rtcp_packets)
    {
        const auto sender_ssrc = require_rtcp_sender_report(packet, cname);
        video_report = video_report || sender_ssrc == *video_ssrc;
        audio_report = audio_report || sender_ssrc == *audio_ssrc;
    }
    require(video_report, "rtcp video sender report");
    require(audio_report, "rtcp audio sender report");
}

void test_webrtc_opus_packetizer()
{
    test_webrtc_opus_channel_count(1);
    test_webrtc_opus_channel_count(2);
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_timebase_conversions();
    std::cout << "[pass] timebase_conversions\n";
    media_server::test_rtmp_timestamp_timeline();
    std::cout << "[pass] rtmp_timestamp_timeline\n";
    media_server::test_internal_format_contract();
    std::cout << "[pass] internal_format_contract\n";
    media_server::test_rtmp_aac_asc_adts_contract();
    std::cout << "[pass] rtmp_aac_asc_adts_contract\n";
    media_server::test_rtmp_input_codec_configuration_updates();
    std::cout << "[pass] rtmp_input_codec_configuration_updates\n";
    media_server::test_rtmp_input_rejects_video_codec_change();
    std::cout << "[pass] rtmp_input_rejects_video_codec_change\n";
    media_server::test_rtmp_rejects_live_playback_control();
    std::cout << "[pass] rtmp_rejects_live_playback_control\n";
    media_server::test_rtmp_output_pull_codecs_and_order();
    std::cout << "[pass] rtmp_output_pull_codecs_and_order\n";
    media_server::test_rtmp_output_config_reset_and_end();
    std::cout << "[pass] rtmp_output_config_reset_and_end\n";
    media_server::test_rtmp_output_runtime_add_audio();
    std::cout << "[pass] rtmp_output_runtime_add_audio\n";
    media_server::test_rtmp_tcp_error_lifecycle();
    std::cout << "[pass] rtmp_tcp_error_lifecycle\n";
    media_server::test_flv_config_cache_lifecycle();
    std::cout << "[pass] flv_config_cache_lifecycle\n";
    media_server::test_h265_output_paths();
    std::cout << "[pass] h265_output_paths\n";
    media_server::test_rtsp_muxer_zero_origin_timeline();
    std::cout << "[pass] rtsp_muxer_zero_origin_timeline\n";
    media_server::test_rtsp_aac_adts_round_trip();
    std::cout << "[pass] rtsp_aac_adts_round_trip\n";
    media_server::test_rtsp_client_session_timeout();
    std::cout << "[pass] rtsp_client_session_timeout\n";
    media_server::test_tcp_connection_shutdown_lifecycle();
    std::cout << "[pass] tcp_connection_shutdown_lifecycle\n";
    media_server::test_tcp_connection_io_error_propagation();
    std::cout << "[pass] tcp_connection_io_error_propagation\n";
    media_server::test_tcp_connection_shutdown_discards_pending_writes();
    std::cout << "[pass] tcp_connection_shutdown_discards_pending_writes\n";
    media_server::test_tcp_listener_startup_error();
    std::cout << "[pass] tcp_listener_startup_error\n";
    media_server::test_tcp_listener_worker_affinity();
    std::cout << "[pass] tcp_listener_worker_affinity\n";
    media_server::test_http_flv_client_disconnect();
    std::cout << "[pass] http_flv_client_disconnect\n";
    media_server::test_http_flv_stream_end_during_write();
    std::cout << "[pass] http_flv_stream_end_during_write\n";
    media_server::test_http_flv_pending_bootstrap_end();
    std::cout << "[pass] http_flv_pending_bootstrap_end\n";
    media_server::test_http_flv_pull_one_frame_and_overrun();
    std::cout << "[pass] http_flv_pull_one_frame_and_overrun\n";
    media_server::test_http_flv_h265_pull();
    std::cout << "[pass] http_flv_h265_pull\n";
    media_server::test_http_flv_fast_and_slow_readers();
    std::cout << "[pass] http_flv_fast_and_slow_readers\n";
    media_server::test_http_flv_audio_video_order();
    std::cout << "[pass] http_flv_audio_video_order\n";
    media_server::test_http_flv_track_reset();
    std::cout << "[pass] http_flv_track_reset\n";
    media_server::test_rtsp_pull_url_contract();
    std::cout << "[pass] rtsp_pull_url_contract\n";
    media_server::test_rtsp_input_selects_single_audio_and_video();
    std::cout << "[pass] rtsp_input_selects_single_audio_and_video\n";
    media_server::test_rtsp_input_media_driven_keepalive();
    std::cout << "[pass] rtsp_input_media_driven_keepalive\n";
    media_server::test_rtsp_client_rejects_empty_media_selection();
    std::cout << "[pass] rtsp_client_rejects_empty_media_selection\n";
    media_server::test_rtsp_output_session_contract();
    std::cout << "[pass] rtsp_output_session_contract\n";
    media_server::test_rtsp_output_h265();
    std::cout << "[pass] rtsp_output_h265\n";
    media_server::test_rtsp_output_setup_track_lifecycle();
    std::cout << "[pass] rtsp_output_setup_track_lifecycle\n";
    media_server::test_rtsp_output_rejects_stale_description();
    std::cout << "[pass] rtsp_output_rejects_stale_description\n";
    media_server::test_media_stream_fanout_and_reentrancy();
    std::cout << "[pass] media_stream_fanout_and_reentrancy\n";
    media_server::test_media_stream_gop_cache();
    std::cout << "[pass] media_stream_gop_cache\n";
    media_server::test_media_stream_worker_fanout();
    std::cout << "[pass] media_stream_worker_fanout\n";
    media_server::test_media_stream_worker_removal();
    std::cout << "[pass] media_stream_worker_removal\n";
    media_server::test_media_stream_pull_reader_overrun();
    std::cout << "[pass] media_stream_pull_reader_overrun\n";
    media_server::test_media_stream_pull_reader_duplicate_read();
    std::cout << "[pass] media_stream_pull_reader_duplicate_read\n";
    media_server::test_media_stream_pull_reader_previous_gop_continuity();
    std::cout << "[pass] media_stream_pull_reader_previous_gop_continuity\n";
    media_server::test_media_stream_add_reader_after_end();
    std::cout << "[pass] media_stream_add_reader_after_end\n";
    media_server::test_media_stream_pull_reader_generation_order();
    std::cout << "[pass] media_stream_pull_reader_generation_order\n";
    media_server::test_stream_registry_generation_lifecycle();
    std::cout << "[pass] stream_registry_generation_lifecycle\n";
    media_server::test_hls_output();
    std::cout << "[pass] hls_output\n";
    media_server::test_hls_service_lifecycle();
    std::cout << "[pass] hls_service_lifecycle\n";
    media_server::test_webrtc_rtp_packetizer();
    std::cout << "[pass] webrtc_rtp_packetizer\n";
    media_server::test_webrtc_opus_packetizer();
    std::cout << "[pass] webrtc_opus_packetizer\n";
    media_server::test_webrtc_output_initialization_failure();
    std::cout << "[pass] webrtc_output_initialization_failure\n";
    media_server::test_webrtc_rtcp_sender();
    std::cout << "[pass] webrtc_rtcp_sender\n";
    std::cout << "all tests passed\n";
    return 0;
}
