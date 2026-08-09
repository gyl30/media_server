#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"
#include "media/hls/hls_service.h"
#include "media/net/tcp_connection.h"
#include "media/net/tcp_listener.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/http/http_flv_output.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/webrtc/webrtc_output.h"

extern "C"
{
#include "amf0.h"
#include "flv-demuxer.h"
#include "flv-header.h"
#include "flv-proto.h"
#include "mpeg-ts.h"
#include "rtmp-chunk-header.h"
#include "rtmp-client.h"
#include "rtmp-msgtypeid.h"
#include "rtp-packet.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "rtsp-muxer.h"
#include "rtsp-payloads.h"
}

#include <algorithm>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};
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

class rtmp_output_test_peer final
{
   public:
    rtmp_output_test_peer() : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_socket_(io_)
    {
        stream_ = std::make_shared<media_stream>("live/camera");
        require(stream_->update_track(make_video_track()), "rtmp output video track");
        require(registry_.add(stream_), "rtmp output registry add");

        client_socket_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        session_ = std::make_shared<rtmp_session>(std::move(connection), registry_);
        session_->start();
        runner_ = std::jthread([this]() { io_.run(); });

        rtmp_client_handler_t handler{};
        handler.send = &rtmp_output_test_peer::send_callback;
        handler.onvideo = &rtmp_output_test_peer::video_callback;
        handler.onaudio = &rtmp_output_test_peer::media_callback;
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
        io_.stop();
        runner_.join();
        session_->on_end();
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
        require(video.codecid == FLV_VIDEO_H264 && video.avpacket == FLV_SEQUENCE_HEADER, "rtmp output video config");
        static_cast<rtmp_output_test_peer*>(param)->received_video_config_ = true;
        return 0;
    }

    static int media_callback(void*, const void*, std::size_t, std::uint32_t) { return 0; }

    void receive_until_video_config()
    {
        std::array<std::uint8_t, 8 * 1024> data{};
        while (!received_video_config_)
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
    bool received_video_config_{};
    std::jthread runner_;
};

void test_rtmp_rejects_live_playback_control()
{
    rtmp_output_test_peer peer;
    const auto pause = peer.pause();
    const auto seek = peer.seek();
    require(pause.level == "error" && pause.code == "NetStream.Pause.Failed", "rtmp live pause rejected");
    require(seek.level == "error" && seek.code == "NetStream.Seek.Failed", "rtmp live seek rejected");
}

void test_tcp_listener_startup_error()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor occupied(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

    tcp_listener listener(io, occupied.local_endpoint().port(), [](boost::asio::ip::tcp::socket) {});
    require(static_cast<bool>(listener.start()), "tcp listener reports bind failure");
}

void test_rtsp_pull_url_contract()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

    boost::asio::io_context client_io;
    stream_registry registry;
    auto invalid = std::make_shared<rtsp_input_session>(client_io, registry, "relay/invalid", "rtsp://127.0.0.1:99999/live/test");
    require(!invalid->start(), "rtsp invalid port rejected");
    require(!registry.find("relay/invalid"), "rtsp invalid url leaves registry unchanged");

    const auto port = acceptor.local_endpoint().port();
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/test";
    const auto credential_url = "rtsp://us%65r:p%40ss@127.0.0.1:" + std::to_string(port) + "/live/test";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/auth", credential_url);
    require(pull->start(), "rtsp auth pull start");

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
}

void test_rtsp_input_selects_single_audio_and_video()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/multi";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/single-av", request_url);
    require(pull->start(), "rtsp single audio video pull start");
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

class rtsp_output_test_peer final
{
   public:
    explicit rtsp_output_test_peer(bool h265 = false) : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_(io_)
    {
        stream_ = std::make_shared<media_stream>("live/test");
        require(stream_->update_track(h265 ? make_h265_track() : make_video_track()), "rtsp output video track");
        require(stream_->update_track(make_audio_track()), "rtsp output audio track");
        require(registry_.add(stream_), "rtsp output registry add");

        client_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        session_ = std::make_shared<rtsp_output_session>(std::move(connection), registry_, acceptor_.local_endpoint().port());
        session_->start();
        runner_ = std::jthread([this]() { io_.run(); });
    }

    ~rtsp_output_test_peer()
    {
        boost::system::error_code error;
        client_.close(error);
        io_.stop();
    }

    std::string request(std::string_view request)
    {
        boost::asio::write(client_, boost::asio::buffer(request));
        std::string response;
        boost::asio::read_until(client_, boost::asio::dynamic_buffer(response), "\r\n\r\n");
        const auto header_end = response.find("\r\n\r\n");
        require(header_end != std::string::npos, "rtsp response header");
        const auto total = header_end + 4U + rtsp_content_length(response);
        if (response.size() < total)
        {
            boost::asio::read(client_, boost::asio::dynamic_buffer(response), boost::asio::transfer_exactly(total - response.size()));
        }
        return response;
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    [[nodiscard]] std::shared_ptr<media_stream> stream() const { return stream_; }

   private:
    boost::asio::io_context io_;
    stream_registry registry_;
    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket client_;
    std::shared_ptr<rtsp_output_session> session_;
    std::jthread runner_;
};

void test_rtsp_output_session_contract()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";

    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp output describe");
    require(describe.find("a=control:trackID=1\r\n") != std::string::npos, "rtsp output video control");
    require(describe.find("a=control:trackID=2\r\n") != std::string::npos, "rtsp output audio control");

    const auto wrong_stream = peer.request("SETUP rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                           "/live/other/trackID=1 RTSP/1.0\r\n"
                                           "CSeq: 2\r\n"
                                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(wrong_stream.starts_with("RTSP/1.0 404"), "rtsp output setup stream identity");

    const auto video_setup = peer.request("SETUP " + base +
                                          "/trackID=1 RTSP/1.0\r\n"
                                          "CSeq: 3\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(video_setup.starts_with("RTSP/1.0 200"), "rtsp output video setup");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(!session.empty(), "rtsp output session id");

    const auto duplicate_setup = peer.request("SETUP " + base +
                                              "/trackID=1 RTSP/1.0\r\n"
                                              "CSeq: 4\r\n"
                                              "Session: " +
                                              session +
                                              "\r\n"
                                              "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(duplicate_setup.starts_with("RTSP/1.0 200"), "rtsp output idempotent setup");

    const auto wrong_session = peer.request("SETUP " + base +
                                            "/trackID=2 RTSP/1.0\r\n"
                                            "CSeq: 5\r\n"
                                            "Session: wrong\r\n"
                                            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(wrong_session.starts_with("RTSP/1.0 454"), "rtsp output setup session identity");

    const auto channel_conflict = peer.request("SETUP " + base +
                                               "/trackID=2 RTSP/1.0\r\n"
                                               "CSeq: 6\r\n"
                                               "Session: " +
                                               session +
                                               "\r\n"
                                               "Transport: RTP/AVP/TCP;unicast;interleaved=1-2\r\n\r\n");
    require(channel_conflict.starts_with("RTSP/1.0 461"), "rtsp output interleaved channel conflict");

    const auto audio_setup = peer.request("SETUP " + base +
                                          "/trackID=2 RTSP/1.0\r\n"
                                          "CSeq: 7\r\n"
                                          "Session: " +
                                          session +
                                          "\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(audio_setup.starts_with("RTSP/1.0 200"), "rtsp output audio setup");

    const auto wrong_play = peer.request("PLAY rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                         "/live/other RTSP/1.0\r\n"
                                         "CSeq: 8\r\n"
                                         "Session: " +
                                         session + "\r\n\r\n");
    require(wrong_play.starts_with("RTSP/1.0 404"), "rtsp output play stream identity");

    const auto play = peer.request("PLAY " + base +
                                   " RTSP/1.0\r\n"
                                   "CSeq: 9\r\n"
                                   "Session: " +
                                   session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp output play");

    const auto late_setup = peer.request("SETUP " + base +
                                         "/trackID=1 RTSP/1.0\r\n"
                                         "CSeq: 10\r\n"
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
    require(peer.stream()->update_track(std::move(updated)), "rtsp stale source config update");

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

    require(stream.add_sink(first), "add first sink");
    require(stream.add_sink(removing), "add removing sink");
    require(stream.add_sink(second), "add second sink");
    require(first->tracks == 1 && second->tracks == 1, "late sink track snapshot");

    require(stream.publish(make_video_frame(0, true)), "publish first frame");
    require(removing->frames == 1, "self-removing sink receives first frame");
    require(first->frames == 1 && second->frames == 1, "other sinks survive reentrant remove");

    require(stream.publish(make_video_frame(40'000'000, false)), "publish second frame");
    require(removing->frames == 1, "removed sink not called again");
    require(first->frames == 2 && second->frames == 2, "remaining sinks continue");

    stream.end();
    require(first->ends == 1 && second->ends == 1, "stream end fanout");

    media_stream track_remove_stream("live/track-remove");
    require(track_remove_stream.update_track(make_video_track()), "track remove video");
    require(track_remove_stream.update_track(make_audio_track()), "track remove audio");
    auto track_removing = std::make_shared<track_removing_sink>(track_remove_stream);
    require(!track_remove_stream.add_sink(track_removing), "track callback removes pending sink");
    require(track_removing->tracks == 1, "removed sink stops track replay");
    require(track_remove_stream.publish(make_video_frame(0, true)), "track remove publish");
    require(track_removing->frames == 0, "track removed sink receives no frame");

    media_stream track_end_stream("live/track-end");
    require(track_end_stream.update_track(make_video_track()), "track end video");
    require(track_end_stream.update_track(make_audio_track()), "track end audio");
    auto track_ending = std::make_shared<track_ending_sink>(track_end_stream);
    require(!track_end_stream.add_sink(track_ending), "track callback ends stream");
    require(track_end_stream.ended(), "track callback stream ended");
    require(track_ending->tracks == 1, "ended stream stops track replay");
    require(track_ending->ends == 1, "new sink receives end during track replay");

    media_stream update_remove_stream("live/update-remove");
    auto update_removed = std::make_shared<counting_sink>();
    auto update_removing = std::make_shared<track_other_removing_sink>(update_remove_stream, *update_removed);
    require(update_remove_stream.add_sink(update_removing), "add update removing sink");
    require(update_remove_stream.add_sink(update_removed), "add update removed sink");
    require(update_remove_stream.update_track(make_video_track()), "update removes later sink");
    require(update_removing->tracks == 1, "update removing sink receives track");
    require(update_removed->tracks == 0, "update removed sink skips current track");

    media_stream publish_remove_stream("live/publish-remove");
    require(publish_remove_stream.update_track(make_video_track()), "publish remove video");
    auto publish_removed = std::make_shared<counting_sink>();
    auto publish_removing = std::make_shared<frame_other_removing_sink>(publish_remove_stream, *publish_removed);
    require(publish_remove_stream.add_sink(publish_removing), "add publish removing sink");
    require(publish_remove_stream.add_sink(publish_removed), "add publish removed sink");
    require(publish_remove_stream.publish(make_video_frame(0, true)), "publish removes later sink");
    require(publish_removing->frames == 1, "publish removing sink receives frame");
    require(publish_removed->frames == 0, "publish removed sink skips current frame");

    media_stream update_end_stream("live/update-end");
    auto update_ending = std::make_shared<track_ending_sink>(update_end_stream);
    auto update_after = std::make_shared<counting_sink>();
    require(update_end_stream.add_sink(update_ending), "add update ending sink");
    require(update_end_stream.add_sink(update_after), "add update trailing sink");
    require(update_end_stream.update_track(make_video_track()), "update callback ends stream");
    require(update_end_stream.ended(), "update callback stream ended");
    require(update_ending->tracks == 1 && update_ending->ends == 1, "update ending sink lifecycle");
    require(update_after->tracks == 0 && update_after->ends == 1, "update stops callbacks after end");

    media_stream publish_end_stream("live/publish-end");
    require(publish_end_stream.update_track(make_video_track()), "publish end video");
    auto frame_ending = std::make_shared<frame_ending_sink>(publish_end_stream);
    auto frame_after = std::make_shared<counting_sink>();
    require(publish_end_stream.add_sink(frame_ending), "add frame ending sink");
    require(publish_end_stream.add_sink(frame_after), "add frame trailing sink");
    require(publish_end_stream.publish(make_video_frame(0, true)), "frame callback ends stream");
    require(publish_end_stream.ended(), "frame callback stream ended");
    require(frame_ending->frames == 1 && frame_ending->ends == 1, "frame ending sink lifecycle");
    require(frame_after->frames == 0 && frame_after->ends == 1, "publish stops callbacks after end");

    media_stream version_stream("live/version");
    auto first_version = make_video_track();
    first_version.config_version = 99;
    require(version_stream.update_track(std::move(first_version)), "version first track");
    require(version_stream.tracks().front().config_version == 1, "stream owns initial config version");
    auto version_observer = std::make_shared<track_version_sink>();
    require(version_stream.add_sink(version_observer), "version observer add");
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
    require(update_reentrant_stream.add_sink(update_reentrant), "reentrant updater add");
    require(update_reentrant_stream.add_sink(update_observer), "reentrant observer add");
    require(update_reentrant_stream.update_track(make_video_track(2)), "reentrant outer update");
    require(update_reentrant->update_succeeded, "reentrant nested update");
    require(update_observer->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {video_track_id, 3}},
            "reentrant stale update skipped");

    media_stream replay_reentrant_stream("live/replay-reentrant");
    require(replay_reentrant_stream.update_track(make_video_track()), "replay video track");
    require(replay_reentrant_stream.update_track(make_audio_track()), "replay audio track");
    auto replay_reentrant = std::make_shared<track_updating_sink>(replay_reentrant_stream, video_track_id, 1, make_audio_track(2));
    require(replay_reentrant_stream.add_sink(replay_reentrant), "replay reentrant sink add");
    require(replay_reentrant->update_succeeded, "replay nested update");
    require(replay_reentrant->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {audio_track_id, 2}},
            "replay stale track skipped");
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
    require(first->add_sink(replacing), "registry generation replacement observer add");
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

    std::size_t flv_end_count = 0;
    std::size_t flv_bytes = 0;
    media_stream flv_stream("live/http-flv");
    require(flv_stream.update_track(make_video_track()), "http flv initial track config");
    const auto flv_output = std::make_shared<http_flv_output>(
        flv_stream.tracks(), [&flv_bytes](std::span<const std::uint8_t> data) { flv_bytes += data.size(); }, [&flv_end_count]() { ++flv_end_count; });
    require(flv_stream.add_sink(flv_output), "http flv add sink");
    const auto first_config_bytes = flv_bytes;
    auto updated_flv_video = make_video_track();
    updated_flv_video.codec_config = h264_config_updated;
    require(flv_stream.update_track(std::move(updated_flv_video)), "http flv update track config");
    require(flv_bytes > first_config_bytes, "http flv writes updated track config");
    require(flv_end_count == 0U, "http flv existing track config update");
    require(flv_stream.update_track(make_audio_track()), "http flv topology change");
    flv_output->on_track(make_audio_track());
    require(flv_end_count == 1U, "http flv topology change closes once");
}

void test_hls_service_lifecycle()
{
    stream_registry registry;
    hls_service hls(registry, hls_config{.target_duration_seconds = 1.0, .window_size = 4});

    auto first = std::make_shared<media_stream>("live/hls");
    require(registry.add(first), "hls first stream add");
    require(first->update_track(make_video_track()), "hls first track");
    require(hls.segment_count("live/hls") == 0U, "hls first output create");

    require(first->publish(make_video_frame(0, true)), "hls first key frame");
    require(first->publish(make_video_frame(1'000'000'000, true)), "hls first segment boundary");
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
    media_server::test_rtmp_rejects_live_playback_control();
    std::cout << "[pass] rtmp_rejects_live_playback_control\n";
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
    media_server::test_tcp_listener_startup_error();
    std::cout << "[pass] tcp_listener_startup_error\n";
    media_server::test_rtsp_pull_url_contract();
    std::cout << "[pass] rtsp_pull_url_contract\n";
    media_server::test_rtsp_input_selects_single_audio_and_video();
    std::cout << "[pass] rtsp_input_selects_single_audio_and_video\n";
    media_server::test_rtsp_client_rejects_empty_media_selection();
    std::cout << "[pass] rtsp_client_rejects_empty_media_selection\n";
    media_server::test_rtsp_output_session_contract();
    std::cout << "[pass] rtsp_output_session_contract\n";
    media_server::test_rtsp_output_h265();
    std::cout << "[pass] rtsp_output_h265\n";
    media_server::test_rtsp_output_rejects_stale_description();
    std::cout << "[pass] rtsp_output_rejects_stale_description\n";
    media_server::test_media_stream_fanout_and_reentrancy();
    std::cout << "[pass] media_stream_fanout_and_reentrancy\n";
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
