#include "media/codec/audio_transcoder.h"
#include "media/codec/codec_utils.h"
#include "media/codec/video_transcoder.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"
#include "media/hls/hls_service.h"
#include "media/net/tcp_connection.h"
#include "media/net/io_context_pool.h"
#include "media/net/tcp_listener.h"
#include "media/net/udp_socket.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/rtsp/rtsp_server.h"
#include "media/http/http_flv_output.h"
#include "media/http/http_session.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/webrtc/webrtc_output.h"
#include "media/webrtc/whep_service.h"

#include <cstddef>

extern "C"
{
#include "aom-av1.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>

#include "amf0.h"
#include "avpbs.h"
#include "flv-demuxer.h"
#include "flv-header.h"
#include "flv-muxer.h"
#include "flv-parser.h"
#include "flv-proto.h"
#include "mpeg-ts.h"
#include "mov-buffer.h"
#include "mov-format.h"
#include "mov-reader.h"
#include "opus-head.h"
#include "rtmp-chunk-header.h"
#include "rtmp-client.h"
#include "rtmp-internal.h"
#include "rtmp-msgtypeid.h"
#include "rtp-ext.h"
#include "rtp-packet.h"
#include "rtp-payload.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "rtsp-muxer.h"
#include "rtsp-payloads.h"
#include "sdp-payload.h"
}

#include <algorithm>
#include <atomic>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
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

[[noreturn]] void fail(std::string_view message);
void require(bool condition, std::string_view message);

struct encoded_video_fixture
{
    codec_id codec{};
    int width{};
    int height{};
    std::vector<std::uint8_t> codec_config;
    std::vector<media_frame> frames;
};

encoded_video_fixture make_video_transcoder_fixture(codec_id codec, int requested_width = 64, int requested_height = 48)
{
    const int width = requested_width;
    const int height = requested_height;
    constexpr int frame_count = 5;
    constexpr AVRational source_time_base{1, 25};
    constexpr std::int64_t origin_ns = 5'000'000'000;

    if (codec == codec_id::h265)
    {
        const std::array<std::vector<std::uint8_t>, frame_count> payloads{
            std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x28, 0x01, 0xac, 0x76, 0x23, 0xef, 0x90, 0x4d, 0x80,
                                      0x71, 0x27, 0x9f, 0x74, 0xc1, 0x2d, 0x86, 0xe1, 0x55, 0x25, 0xeb, 0x9f, 0x73, 0xf8},
            std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xd0, 0x11, 0x56, 0x20, 0xe4, 0x49, 0x2a, 0x44, 0x87,
                                      0x11, 0x8c, 0x7a, 0x46, 0x46, 0x4d, 0xdb, 0x7e, 0x49, 0x4c, 0xc8, 0x76, 0xb7, 0x8c},
            std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xe0, 0x24, 0xbe, 0x08, 0x14, 0xc0, 0xd2, 0xa9},
            std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xd0, 0x21, 0xd5, 0x62, 0x0e, 0xc0, 0x26, 0xa0, 0x73,
                                      0xc6, 0x35, 0x6e, 0xdd, 0x23, 0xc3, 0xc7, 0xd6, 0x1a, 0x00, 0xa3, 0x66, 0x30},
            std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xe0, 0x66, 0xb5, 0xe0, 0x81, 0x44, 0xd2, 0xa9},
        };
        const std::array<std::int64_t, frame_count> dts{4'920'000'000, 4'960'000'000, 5'000'000'000, 5'040'000'000, 5'080'000'000};
        const std::array<std::int64_t, frame_count> pts{5'000'000'000, 5'080'000'000, 5'040'000'000, 5'160'000'000, 5'120'000'000};
        encoded_video_fixture fixture{
            .codec = codec,
            .width = width,
            .height = height,
            .codec_config = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03,
                             0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x1e, 0x91, 0x30, 0x24, 0x00, 0x00,
                             0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
                             0x00, 0x00, 0x03, 0x00, 0x1e, 0xa0, 0x20, 0x83, 0x16, 0x59, 0x13, 0x4a, 0x4c, 0x2e, 0x68,
                             0x08, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x40, 0x00, 0x00, 0x00,
                             0x01, 0x44, 0x01, 0xc0, 0x73, 0xc0, 0x89},
            .frames = {},
        };
        for (std::size_t index = 0; index < payloads.size(); ++index)
        {
            fixture.frames.push_back(media_frame{
                .track = video_track_id,
                .dts_ns = dts[index],
                .pts_ns = pts[index],
                .key_frame = index == 0,
                .payload = std::make_shared<const std::vector<std::uint8_t>>(payloads[index]),
            });
        }
        return fixture;
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    require(encoder != nullptr, "video transcoder fixture encoder");
    AVCodecContext* context = avcodec_alloc_context3(encoder);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    require(context != nullptr && frame != nullptr && packet != nullptr, "video transcoder fixture allocate");

    context->width = width;
    context->height = height;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = source_time_base;
    context->framerate = AVRational{25, 1};
    context->thread_count = 1;
    context->gop_size = frame_count;
    context->max_b_frames = 1;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    context->color_range = AVCOL_RANGE_JPEG;
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_BT709;
    context->colorspace = AVCOL_SPC_BT709;
    context->chroma_sample_location = AVCHROMA_LOC_LEFT;
    require(av_opt_set(context->priv_data, "preset", "ultrafast", 0) == 0, "video transcoder fixture preset");
    require(avcodec_open2(context, encoder, nullptr) == 0, "video transcoder fixture encoder open");
    require(context->extradata != nullptr && context->extradata_size > 4, "video transcoder fixture extradata");

    encoded_video_fixture fixture{
        .codec = codec,
        .width = width,
        .height = height,
        .codec_config = {context->extradata, context->extradata + context->extradata_size},
        .frames = {},
    };
    require(fixture.codec_config[0] == 0 && fixture.codec_config[1] == 0 &&
                (fixture.codec_config[2] == 1 || (fixture.codec_config[2] == 0 && fixture.codec_config[3] == 1)),
            "video transcoder fixture annex-b config");

    frame->format = context->pix_fmt;
    frame->width = width;
    frame->height = height;
    require(av_frame_get_buffer(frame, 32) == 0, "video transcoder fixture frame buffer");

    for (int index = 0; index < frame_count; ++index)
    {
        require(av_frame_make_writable(frame) == 0, "video transcoder fixture writable");
        for (int y = 0; y < height; ++y)
        {
            std::fill_n(frame->data[0] + y * frame->linesize[0], width, static_cast<std::uint8_t>(y < height / 2 ? 0 : 255));
        }
        for (int y = 0; y < height / 2; ++y)
        {
            std::fill_n(frame->data[1] + y * frame->linesize[1], width / 2, static_cast<std::uint8_t>(128));
            std::fill_n(frame->data[2] + y * frame->linesize[2], width / 2, static_cast<std::uint8_t>(128));
        }
        frame->pts = index;
        require(avcodec_send_frame(context, frame) == 0, "video transcoder fixture send frame");
        while (true)
        {
            av_packet_unref(packet);
            const int result = avcodec_receive_packet(context, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                break;
            }
            require(result == 0, "video transcoder fixture receive packet");
            fixture.frames.push_back(media_frame{
                .track = video_track_id,
                .dts_ns = origin_ns + av_rescale_q(packet->dts, source_time_base, AVRational{1, 1'000'000'000}),
                .pts_ns = origin_ns + av_rescale_q(packet->pts, source_time_base, AVRational{1, 1'000'000'000}),
                .key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0,
                .payload = std::make_shared<const std::vector<std::uint8_t>>(packet->data, packet->data + packet->size),
            });
        }
    }

    require(avcodec_send_frame(context, nullptr) == 0, "video transcoder fixture flush");
    while (true)
    {
        av_packet_unref(packet);
        const int result = avcodec_receive_packet(context, packet);
        if (result == AVERROR_EOF)
        {
            break;
        }
        require(result == 0, "video transcoder fixture flush receive");
        fixture.frames.push_back(media_frame{
            .track = video_track_id,
            .dts_ns = origin_ns + av_rescale_q(packet->dts, source_time_base, AVRational{1, 1'000'000'000}),
            .pts_ns = origin_ns + av_rescale_q(packet->pts, source_time_base, AVRational{1, 1'000'000'000}),
            .key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(packet->data, packet->data + packet->size),
        });
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    require(fixture.frames.size() == frame_count, "video transcoder fixture packet count");
    for (const auto& encoded : fixture.frames)
    {
        require(encoded.payload && encoded.payload->size() >= 4 && (*encoded.payload)[0] == 0 && (*encoded.payload)[1] == 0 &&
                    ((*encoded.payload)[2] == 1 || ((*encoded.payload)[2] == 0 && (*encoded.payload)[3] == 1)),
                "video transcoder fixture annex-b frame");
    }
    return fixture;
}

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
    std::uint64_t generation{};
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

media_track make_opus_track(std::uint16_t channel_count = 2)
{
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::opus,
        .clock_rate = 48'000,
        .channel_count = channel_count,
        .codec_config = {},
    };
}

media_track make_g711_track(codec_id codec)
{
    require(codec == codec_id::g711a || codec == codec_id::g711u, "g711 track codec");
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec,
        .clock_rate = 8'000,
        .channel_count = 1,
        .codec_config = {},
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

media_frame make_video_frame(std::int64_t pts_ns, bool key_frame, std::span<const std::uint8_t> codec_config)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes.assign(codec_config.begin(), codec_config.end());
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

media_frame make_video_frame(std::int64_t pts_ns, bool key_frame)
{
    return make_video_frame(pts_ns, key_frame, h264_config);
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

media_frame make_h265_frame(std::int64_t pts_ns, bool key_frame, std::span<const std::uint8_t> codec_config)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes.assign(codec_config.begin(), codec_config.end());
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

media_frame make_h265_frame(std::int64_t pts_ns, bool key_frame)
{
    return make_h265_frame(pts_ns, key_frame, h265_config);
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

media_frame make_raw_audio_frame(std::int64_t pts_ns, std::vector<std::uint8_t> payload)
{
    return media_frame{
        .track = audio_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
    };
}

media_frame make_opus_frame(std::int64_t pts_ns, std::vector<std::uint8_t> payload = {0xf8, 0xff, 0xfe})
{
    return make_raw_audio_frame(pts_ns, std::move(payload));
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

class raw_audio_capture_sink final : public media_sink
{
   public:
    void on_track(const media_track& track) override
    {
        std::scoped_lock lock(mutex_);
        tracks_.push_back(track);
    }

    void on_frame(const media_frame& frame) override
    {
        if (frame.track != audio_track_id || !frame.payload)
        {
            return;
        }
        std::scoped_lock lock(mutex_);
        frames_.push_back(frame);
    }

    void on_end() override {}

    [[nodiscard]] std::vector<media_track> tracks() const
    {
        std::scoped_lock lock(mutex_);
        return tracks_;
    }

    [[nodiscard]] std::vector<media_frame> frames() const
    {
        std::scoped_lock lock(mutex_);
        return frames_;
    }

   private:
    mutable std::mutex mutex_;
    std::vector<media_track> tracks_;
    std::vector<media_frame> frames_;
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
        if (ended_count_.fetch_add(1) + 1 == 1)
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
    explicit pull_test_reader(bool continuous, bool read_on_ready = true, std::vector<track_id> track_ids = {})
        : continuous_(continuous), read_on_ready_(read_on_ready), track_ids_(std::move(track_ids))
    {
    }

    void on_tracks(media_track_snapshot_ptr tracks) override
    {
        const bool changed = apply_tracks(tracks);
        if (changed && read_on_ready_)
        {
            media_reader_cursor cursor;
            {
                std::scoped_lock lock(mutex_);
                cursor = cursor_;
            }
            reader_handle().async_read(cursor);
        }
        condition_.notify_all();
    }

    void on_read(media_read_batch batch) override
    {
        {
            std::scoped_lock lock(mutex_);
            cursor_ = batch.next_cursor;
            batch_sizes_.push_back(batch.entries.size());
        }
        static_cast<void>(apply_tracks(batch.tracks));

        for (auto& entry : batch.entries)
        {
            const auto track = visible_tracks_.find(entry.frame.track);
            if (track == visible_tracks_.end() || track->second.config_version != entry.config_version)
            {
                continue;
            }
            if (waiting_for_key_frame_)
            {
                if (track->second.kind != media_kind::video || !entry.frame.key_frame)
                {
                    continue;
                }
                waiting_for_key_frame_ = false;
            }

            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            frames_.emplace_back(generation_, entry.frame.pts_ns);
        }
        if (continuous_)
        {
            media_reader_cursor cursor;
            {
                std::scoped_lock lock(mutex_);
                cursor = cursor_;
            }
            reader_handle().async_read(cursor);
        }
        condition_.notify_all();
    }

    void on_end() override
    {
        {
            std::scoped_lock lock(mutex_);
            thread_ = std::this_thread::get_id();
            end_generation_ = ++generation_;
            ++ends_;
        }
        condition_.notify_all();
    }

    void request()
    {
        media_reader_cursor cursor;
        {
            std::scoped_lock lock(mutex_);
            cursor = cursor_;
        }
        reader_handle().async_read(cursor);
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

    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::int64_t>> frames() const
    {
        std::scoped_lock lock(mutex_);
        return frames_;
    }

    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> track_versions() const
    {
        std::scoped_lock lock(mutex_);
        return track_versions_;
    }

    [[nodiscard]] std::vector<std::uint64_t> ready_generations() const
    {
        std::scoped_lock lock(mutex_);
        return ready_generations_;
    }

    [[nodiscard]] std::vector<std::size_t> batch_sizes() const
    {
        std::scoped_lock lock(mutex_);
        return batch_sizes_;
    }

    [[nodiscard]] std::size_t ends() const
    {
        std::scoped_lock lock(mutex_);
        return ends_;
    }

    [[nodiscard]] std::uint64_t end_generation() const
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
    [[nodiscard]] bool interested(track_id id) const
    {
        return track_ids_.empty() || std::ranges::find(track_ids_, id) != track_ids_.end();
    }

    bool apply_tracks(const media_track_snapshot_ptr& tracks)
    {
        if (!tracks || tracks->revision <= track_revision_)
        {
            return false;
        }

        std::map<track_id, media_track> visible;
        for (const auto& track : tracks->tracks)
        {
            if (interested(track.id))
            {
                visible.emplace(track.id, track);
            }
        }

        bool changed = visible.size() != visible_tracks_.size();
        bool video_changed = false;
        for (const auto& [id, track] : visible)
        {
            const auto current = visible_tracks_.find(id);
            if (current == visible_tracks_.end() || current->second.config_version != track.config_version)
            {
                changed = true;
                video_changed = video_changed || (current != visible_tracks_.end() && track.kind == media_kind::video);
            }
        }

        track_revision_ = tracks->revision;
        if (!changed)
        {
            return false;
        }

        visible_tracks_ = std::move(visible);
        waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
        {
            std::scoped_lock lock(mutex_);
            ++generation_;
            thread_ = std::this_thread::get_id();
            for (const auto& [id, track] : visible_tracks_)
            {
                static_cast<void>(id);
                track_versions_.emplace_back(generation_, track.config_version);
            }
            ready_generations_.push_back(generation_);
        }
        return true;
    }

    bool continuous_{};
    bool read_on_ready_{};
    std::vector<track_id> track_ids_;
    std::map<track_id, media_track> visible_tracks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread::id thread_;
    std::uint64_t generation_{};
    std::uint64_t end_generation_{};
    media_reader_cursor cursor_;
    std::uint64_t track_revision_{};
    std::size_t ends_{};
    bool waiting_for_key_frame_{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> track_versions_;
    std::vector<std::uint64_t> ready_generations_;
    std::vector<std::pair<std::uint64_t, std::int64_t>> frames_;
    std::vector<std::size_t> batch_sizes_;
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
    explicit rtmp_input_test_peer(std::string stream_name,
                                  std::chrono::milliseconds initial_tracks_timeout = std::chrono::milliseconds{15'000})
        : work_(boost::asio::make_work_guard(io_)), acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_socket_(io_), stream_name_(std::move(stream_name))
    {
        client_socket_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        auto session = std::make_shared<rtmp_session>(std::move(connection), registry_, output_video_config{}, initial_tracks_timeout);
        session_ = session;
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

    void push_metadata(bool audio, codec_id video_codec = codec_id::h264)
    {
        std::array<std::uint8_t, 256> data{};
        auto* current = AMFWriteString(data.data(), data.data() + data.size(), "onMetaData", 10);
        current = AMFWriteECMAArarry(current, data.data() + data.size());
        const auto codec = video_codec == codec_id::h264 ? FLV_VIDEO_H264 : FLV_VIDEO_H265;
        current = AMFWriteNamedDouble(current, data.data() + data.size(), "videocodecid", 12, codec);
        if (audio)
        {
            current = AMFWriteNamedDouble(current, data.data() + data.size(), "audiocodecid", 12, FLV_AUDIO_AAC >> 4);
        }
        current = AMFWriteObjectEnd(current, data.data() + data.size());
        require(current != nullptr, "rtmp input metadata encode");
        require(rtmp_client_push_script(client_, data.data(), static_cast<std::size_t>(current - data.data()), 0) == 0,
                "rtmp input push metadata");
    }

    void push_video_config(media_track track)
    {
        const auto packet = make_rtmp_video_sequence_header(std::move(track));
        require(rtmp_client_push_video(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push video config");
    }

    void push_raw_video(codec_id codec)
    {
        std::vector<std::uint8_t> packet;
        flv_output_muxer muxer(
            [&packet](int type, std::span<const std::uint8_t> data, std::uint32_t)
            {
                if (type == FLV_TYPE_VIDEO)
                {
                    packet.assign(data.begin(), data.end());
                }
            });
        muxer.on_track(codec == codec_id::h264 ? make_video_track() : make_h265_track());
        packet.clear();
        muxer.on_frame(codec == codec_id::h264 ? make_video_frame(0, true) : make_h265_frame(0, true));
        require(!packet.empty(), "rtmp input raw video packet");
        require(rtmp_client_push_video(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push raw video");
    }

    void push_audio_config(std::span<const std::uint8_t> asc)
    {
        std::vector<std::uint8_t> packet{0xaf, FLV_SEQUENCE_HEADER};
        packet.insert(packet.end(), asc.begin(), asc.end());
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push audio config");
    }

    void push_opus_config(std::uint16_t channel_count = 2)
    {
        std::vector<std::uint8_t> packet;
        flv_output_muxer muxer(
            [&packet](int type, std::span<const std::uint8_t> data, std::uint32_t)
            {
                if (type == FLV_TYPE_AUDIO && packet.empty())
                {
                    packet.assign(data.begin(), data.end());
                }
            });
        muxer.on_track(make_opus_track(channel_count));
        require(!packet.empty(), "rtmp input opus config");
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push opus config");
    }

    void push_raw_opus(std::uint32_t timestamp, std::vector<std::uint8_t> payload = {0xf8, 0xff, 0xfe})
    {
        std::vector<std::uint8_t> packet;
        flv_output_muxer muxer(
            [&packet](int type, std::span<const std::uint8_t> data, std::uint32_t)
            {
                if (type == FLV_TYPE_AUDIO)
                {
                    packet.assign(data.begin(), data.end());
                }
            });
        muxer.on_track(make_opus_track(1));
        packet.clear();
        muxer.on_frame(make_opus_frame(static_cast<std::int64_t>(timestamp) * 1'000'000, std::move(payload)));
        require(!packet.empty(), "rtmp input raw opus packet");
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), timestamp) == 0, "rtmp input push raw opus");
    }

    void push_g711(codec_id codec, std::uint32_t timestamp = 0)
    {
        std::vector<std::uint8_t> packet;
        flv_output_muxer muxer(
            [&packet](int type, std::span<const std::uint8_t> data, std::uint32_t)
            {
                if (type == FLV_TYPE_AUDIO)
                {
                    packet.assign(data.begin(), data.end());
                }
            });
        muxer.on_track(make_g711_track(codec));
        muxer.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = 0,
            .pts_ns = 0,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(160U, codec == codec_id::g711a ? 0xd5U : 0xffU),
        });
        require(!packet.empty(), "rtmp input g711 packet");
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), timestamp) == 0, "rtmp input push g711");
    }

    void push_raw_aac()
    {
        const std::array<std::uint8_t, 4> packet{0xaf, FLV_AVPACKET, 0x11, 0x22};
        require(rtmp_client_push_audio(client_, packet.data(), packet.size(), 0) == 0, "rtmp input push raw aac");
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

    std::size_t track_count()
    {
        std::promise<std::size_t> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [this, &promise]()
                          {
                              const auto stream = registry_.find(stream_name_);
                              promise.set_value(stream ? stream->tracks().size() : 0U);
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtmp input track count query");
        return future.get();
    }

    bool stream_exists() { return query_stream_exists(); }

    std::shared_ptr<raw_audio_capture_sink> attach_audio_capture()
    {
        auto sink = std::make_shared<raw_audio_capture_sink>();
        std::promise<bool> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [this, sink, &promise]()
                          {
                              const auto stream = registry_.find(stream_name_);
                              if (stream)
                              {
                                  stream->add_sink(sink);
                              }
                              promise.set_value(static_cast<bool>(stream));
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready && future.get(), "rtmp input attach audio capture");
        return sink;
    }

    void wait_audio_frames(const std::shared_ptr<raw_audio_capture_sink>& sink, std::size_t count)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (sink->frames().size() >= count)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fail("rtmp input audio frame capture");
    }

    void wait_session_closed()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!session_.expired() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(session_.expired(), "rtmp input session closed");
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
    std::weak_ptr<rtmp_session> session_;
    std::jthread runner_;
};

class rtmp_output_test_peer final
{
   public:
    explicit rtmp_output_test_peer(media_track video_track = make_video_track(), bool with_audio = false)
        : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_socket_(io_), expected_video_codec_(video_track.codec)
    {
        stream_ = std::make_shared<media_stream>("live/camera", io_.get_executor());
        std::vector<media_track> tracks;
        tracks.push_back(std::move(video_track));
        if (with_audio)
        {
            tracks.push_back(make_audio_track());
        }
        require(stream_->set_tracks(std::move(tracks)), "rtmp output tracks");
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
    [[nodiscard]] std::size_t video_config_count() const noexcept { return video_config_count_; }
    [[nodiscard]] std::size_t audio_config_count() const noexcept { return audio_config_count_; }

    void update_video_track(media_track track)
    {
        std::promise<bool> promise;
        auto future = promise.get_future();
        boost::asio::post(io_,
                          [stream = stream_, track = std::move(track), &promise]() mutable
                          { promise.set_value(stream->update_track(std::move(track))); });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready && future.get(), "rtmp output config reset");
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

void test_rtmp_input_initial_topology()
{
    {
        rtmp_input_test_peer peer("live/metadata-first");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        require(!peer.stream_exists(), "rtmp metadata first keeps incomplete stream hidden");
        peer.push_audio_config(aac_asc);
        peer.wait_track(make_video_track(), 1);
        peer.wait_track(make_audio_track(), 1);
        require(peer.track_count() == 2U, "rtmp metadata first publishes complete topology");
    }

    {
        rtmp_input_test_peer peer("live/config-first");
        peer.push_video_config(make_video_track());
        peer.push_audio_config(aac_asc);
        require(!peer.stream_exists(), "rtmp config first waits for metadata before publish");
        peer.push_metadata(true);
        peer.wait_track(make_video_track(), 1);
        peer.wait_track(make_audio_track(), 1);
        require(peer.track_count() == 2U, "rtmp config first publishes complete topology");
    }

    {
        rtmp_input_test_peer peer("live/h265-av");
        peer.push_metadata(true, codec_id::h265);
        peer.push_video_config(make_h265_track());
        require(!peer.stream_exists(), "rtmp h265 keeps incomplete stream hidden");
        peer.push_audio_config(aac_asc);
        peer.wait_track(make_h265_track(), 1);
        peer.wait_track(make_audio_track(), 1);
        require(peer.track_count() == 2U, "rtmp h265 publishes complete topology");
    }

    {
        rtmp_input_test_peer peer("live/video-only-fixed");
        const auto video = make_video_track();
        peer.push_metadata(false);
        peer.push_video_config(video);
        peer.wait_track(video, 1);
        require(peer.track_count() == 1U, "rtmp video only publishes fixed topology");
        peer.push_audio_config(aac_asc);
        peer.wait_stream_removed();
    }
}

void test_rtmp_input_initial_tracks_timeout()
{
    rtmp_input_test_peer peer("live/initial-tracks-timeout", std::chrono::milliseconds(100));
    peer.push_metadata(true);
    peer.push_video_config(make_video_track());
    require(!peer.stream_exists(), "rtmp incomplete stream never enters registry");
    peer.wait_session_closed();
    require(!peer.stream_exists(), "rtmp initial tracks timeout leaves registry empty");
}

void test_rtmp_input_codec_configuration_updates()
{
    {
        rtmp_input_test_peer peer("live/h264-config");
        const auto initial = make_video_track();
        peer.push_metadata(false);
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
        peer.push_metadata(false, codec_id::h265);
        peer.push_video_config(initial);
        peer.wait_track(initial, 1);
        auto updated = make_h265_track();
        updated.codec_config = h265_config_updated;
        peer.push_video_config(updated);
        peer.wait_track(updated, 2);
    }

    {
        rtmp_input_test_peer peer("live/aac-config");
        const auto video = make_video_track();
        const auto initial = make_audio_track();
        peer.push_metadata(true);
        peer.push_video_config(video);
        peer.push_audio_config(initial.codec_config);
        peer.wait_track(video, 1);
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
        rtmp_input_test_peer peer("live/pending-switch");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        require(!peer.stream_exists(), "rtmp pending codec switch keeps incomplete stream hidden");
        peer.push_video_config(make_h265_track());
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/h264-switch");
        const auto h264 = make_video_track();
        peer.push_metadata(false);
        peer.push_video_config(h264);
        peer.wait_track(h264, 1);
        peer.push_video_config(make_h265_track());
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/h265-switch");
        const auto h265 = make_h265_track();
        peer.push_metadata(false, codec_id::h265);
        peer.push_video_config(h265);
        peer.wait_track(h265, 1);
        peer.push_video_config(make_video_track());
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/raw-video-pending-switch");
        peer.push_video_config(make_video_track());
        peer.push_raw_video(codec_id::h265);
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/raw-video-switch");
        peer.push_metadata(false);
        peer.push_video_config(make_video_track());
        peer.wait_track(make_video_track(), 1);
        peer.push_raw_video(codec_id::h265);
        peer.wait_stream_removed();
    }
}

void test_rtmp_input_rejects_audio_codec_change()
{
    {
        rtmp_input_test_peer peer("live/g711a-aac-pending");
        peer.push_g711(codec_id::g711a);
        peer.push_audio_config(aac_asc);
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/g711a-raw-aac-pending");
        peer.push_g711(codec_id::g711a);
        peer.push_raw_aac();
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/aac-raw-opus-pending");
        peer.push_audio_config(aac_asc);
        peer.push_raw_opus(0);
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/aac-opus-pending");
        peer.push_audio_config(aac_asc);
        peer.push_opus_config();
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/opus-g711u-pending");
        peer.push_opus_config();
        peer.push_g711(codec_id::g711u);
        peer.wait_session_closed();
    }

    {
        rtmp_input_test_peer peer("live/aac-opus-established");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        peer.push_audio_config(aac_asc);
        peer.wait_track(make_audio_track(), 1);
        peer.push_opus_config();
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/opus-aac-established");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        peer.push_opus_config();
        peer.wait_track(make_opus_track(2), 1);
        peer.push_audio_config(aac_asc);
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/g711a-aac-established");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        peer.push_g711(codec_id::g711a);
        peer.wait_track(make_g711_track(codec_id::g711a), 1);
        peer.push_audio_config(aac_asc);
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/g711a-raw-aac-established");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        peer.push_g711(codec_id::g711a);
        peer.wait_track(make_g711_track(codec_id::g711a), 1);
        peer.push_raw_aac();
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/video-only-opus");
        peer.push_metadata(false);
        peer.push_video_config(make_video_track());
        peer.wait_track(make_video_track(), 1);
        peer.push_opus_config();
        peer.wait_stream_removed();
    }

    {
        rtmp_input_test_peer peer("live/valid-opus");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        peer.push_opus_config(1);
        peer.wait_track(make_opus_track(1), 1);
        const auto capture = peer.attach_audio_capture();
        const std::vector<std::uint8_t> first{0xf8, 0xff, 0xfe};
        const std::vector<std::uint8_t> second{0x78, 0x11, 0x22, 0x33};
        peer.push_raw_opus(20, first);
        peer.push_raw_opus(40, second);
        peer.push_opus_config(2);
        peer.wait_track(make_opus_track(2), 2);
        peer.wait_audio_frames(capture, 2);
        const auto frames = capture->frames();
        require(*frames[0].payload == first && *frames[1].payload == second, "rtmp input raw opus payloads");
    }

    {
        rtmp_input_test_peer peer("live/g711-first-frame");
        peer.push_metadata(true);
        peer.push_video_config(make_video_track());
        constexpr std::uint32_t near_wrap = 0xfffffff0U;
        peer.push_g711(codec_id::g711u, near_wrap);
        peer.wait_track(make_g711_track(codec_id::g711u), 1);
        const auto capture = peer.attach_audio_capture();
        peer.push_g711(codec_id::g711u, 20);
        peer.wait_audio_frames(capture, 1);
        const auto frames = capture->frames();
        require(frames.front().pts_ns == milliseconds_to_ns(static_cast<std::int64_t>(near_wrap) + 36),
                "rtmp input g711 first frame advances shared timeline");
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
    peer.publish(make_video_frame(0, true, h264_config_updated));
    peer.receive_media(1);
    require(peer.video_config_count() == 2U, "rtmp config reset emits video sequence header with next media frame");
    peer.end_stream();
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
    auto listener = std::make_shared<tcp_listener>(workers, occupied.local_endpoint().port());
    require(static_cast<bool>(listener->startup([](boost::asio::ip::tcp::socket) {})), "tcp listener reports bind failure");
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

void test_udp_socket_receive_and_send()
{
    boost::asio::io_context io;
    auto socket = std::make_shared<udp_socket>(io.get_executor());
    boost::asio::ip::udp::socket peer(io, {boost::asio::ip::address_v4::loopback(), 0});
    std::vector<std::uint8_t> received;
    boost::asio::ip::udp::endpoint source;
    boost::system::error_code read_error;
    const std::vector<std::uint8_t> inbound{1, 2, 3, 4};
    const std::vector<std::uint8_t> outbound{5, 6, 7, 8};

    require(socket->startup(
                boost::asio::ip::address_v4::any(),
                [&](boost::system::error_code error,
                    std::span<const std::uint8_t> packet,
                    const boost::asio::ip::udp::endpoint& endpoint)
                {
                    read_error = error;
                    received.assign(packet.begin(), packet.end());
                    source = endpoint;
                },
                {}),
            "udp socket startup");

    std::array<std::uint8_t, 32> peer_buffer{};
    boost::asio::ip::udp::endpoint sender;
    std::vector<std::uint8_t> peer_received;
    peer.async_receive_from(boost::asio::buffer(peer_buffer),
                            sender,
                            [&](boost::system::error_code error, std::size_t bytes)
                            {
                                require(!error, "udp socket peer receive");
                                peer_received.assign(peer_buffer.begin(), peer_buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
                            });

    peer.send_to(boost::asio::buffer(inbound), {boost::asio::ip::address_v4::loopback(), socket->local_port()});
    socket->send(outbound, {boost::asio::ip::address_v4::loopback(), peer.local_endpoint().port()});
    io.run_for(std::chrono::seconds(1));

    require(!read_error && received == inbound, "udp socket receive payload");
    require(source.address() == boost::asio::ip::address_v4::loopback() && source.port() == peer.local_endpoint().port(),
            "udp socket receive endpoint");
    require(peer_received == outbound, "udp socket send payload");

    socket->shutdown();
    io.restart();
    io.run();
}

void test_udp_socket_multi_endpoint_queue()
{
    boost::asio::io_context io;
    auto socket = std::make_shared<udp_socket>(io.get_executor());
    require(socket->startup(boost::asio::ip::address_v4::any(), {}, {}), "udp multi endpoint startup");

    boost::asio::ip::udp::socket first(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket second(io, {boost::asio::ip::address_v4::loopback(), 0});
    std::array<std::uint8_t, 16> first_buffer{};
    std::array<std::uint8_t, 16> second_buffer{};
    boost::asio::ip::udp::endpoint first_sender;
    boost::asio::ip::udp::endpoint second_sender;
    std::vector<std::uint8_t> first_received;
    std::vector<std::uint8_t> second_received;

    std::function<void()> receive_first;
    receive_first = [&]()
    {
        first.async_receive_from(boost::asio::buffer(first_buffer),
                                 first_sender,
                                 [&](boost::system::error_code error, std::size_t bytes)
                                 {
                                     require(!error, "udp first endpoint receive");
                                     first_received.insert(first_received.end(), first_buffer.begin(), first_buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
                                     if (first_received.size() < 2)
                                     {
                                         receive_first();
                                     }
                                 });
    };
    receive_first();
    second.async_receive_from(boost::asio::buffer(second_buffer),
                              second_sender,
                              [&](boost::system::error_code error, std::size_t bytes)
                              {
                                  require(!error, "udp second endpoint receive");
                                  second_received.assign(second_buffer.begin(), second_buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
                              });

    const boost::asio::ip::udp::endpoint first_endpoint{boost::asio::ip::address_v4::loopback(), first.local_endpoint().port()};
    const boost::asio::ip::udp::endpoint second_endpoint{boost::asio::ip::address_v4::loopback(), second.local_endpoint().port()};
    socket->send({0xa1}, first_endpoint);
    socket->send({0xb1}, second_endpoint);
    socket->send({0xa2}, first_endpoint);
    io.run_for(std::chrono::seconds(1));

    require(first_received == std::vector<std::uint8_t>({0xa1, 0xa2}), "udp queued endpoint remains fixed first");
    require(second_received == std::vector<std::uint8_t>({0xb1}), "udp queued endpoint remains fixed second");

    socket->shutdown();
    io.restart();
    io.run();
}

void test_udp_socket_error_and_shutdown_lifecycle()
{
    {
        boost::asio::io_context io;
        auto socket = std::make_shared<udp_socket>(io.get_executor());
        boost::system::error_code write_error;
        require(socket->startup(boost::asio::ip::address_v4::any(), {},
                                [&](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
                                {
                                    write_error = error;
                                    socket->shutdown();
                                }),
                "udp error startup");
        socket->send({0x01}, {boost::asio::ip::address_v6::loopback(), 9});
        io.run();
        require(static_cast<bool>(write_error), "udp socket reports write error");
    }

    {
        boost::asio::io_context io;
        auto socket = std::make_shared<udp_socket>(io.get_executor());
        int read_callback_count = 0;
        int write_error_callback_count = 0;
        require(socket->startup(
                    boost::asio::ip::address_v4::any(),
                    [&](boost::system::error_code, std::span<const std::uint8_t>, const boost::asio::ip::udp::endpoint&)
                    { ++read_callback_count; },
                    [&](boost::system::error_code, const boost::asio::ip::udp::endpoint&) { ++write_error_callback_count; }),
                "udp shutdown startup");
        const std::weak_ptr<udp_socket> weak_socket = socket;
        socket->send(std::vector<std::uint8_t>(1200, 0x5a), {boost::asio::ip::address_v4::loopback(), 9});
        socket->send(std::vector<std::uint8_t>(1200, 0x5b), {boost::asio::ip::address_v4::loopback(), 9});
        socket->shutdown();
        socket->shutdown();
        socket.reset();
        require(!weak_socket.expired(), "udp shutdown keeps self until owner cleanup");
        io.run();
        require(read_callback_count == 0, "udp shutdown suppresses receive cancellation callback");
        require(write_error_callback_count == 0, "udp shutdown suppresses send cancellation error callback");
        require(weak_socket.expired(), "udp shutdown releases pending operations");
    }
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
    auto listener = std::make_shared<tcp_listener>(workers, port);
    weak_listener = listener;
    require(!listener->startup(
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
                }),
            "tcp listener worker startup");

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

void test_tcp_listener_shutdown_lifecycle()
{
    io_context_pool workers(1);
    auto listener = std::make_shared<tcp_listener>(workers, 0);
    require(!listener->startup([](boost::asio::ip::tcp::socket) {}), "tcp listener shutdown startup");
    const std::weak_ptr<tcp_listener> weak_listener = listener;

    listener->shutdown();
    listener->shutdown();
    listener.reset();
    require(!weak_listener.expired(), "tcp listener shutdown keeps self until owner worker cleanup");

    workers.release_work();
    workers.run();

    require(weak_listener.expired(), "tcp listener released after owner worker cleanup");
}

void test_rtmp_server_shutdown_lifecycle()
{
    io_context_pool workers(1);
    stream_registry registry;
    auto server = std::make_shared<rtmp_server>(workers, registry, 0);
    require(!server->startup(), "rtmp server shutdown startup");
    const std::weak_ptr<rtmp_server> weak_server = server;

    server->shutdown();
    server.reset();
    require(weak_server.expired(), "rtmp server listener callback does not keep server alive");

    workers.release_work();
    workers.run();
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

    auto stream = std::make_shared<media_stream>("live/http-flv-disconnect", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv disconnect track");
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

    auto stream = std::make_shared<media_stream>("live/http-flv-end-write", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv end write track");
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

    auto stream = std::make_shared<media_stream>("live/http-flv-pending-end", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv pending end track");
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

void test_http_flv_batch_consumption_and_overrun()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-pull", reader_worker.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv pull video track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
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
    require(capture.writes.size() == 2U && !capture.writes.back().bootstrap, "http flv starts current batch after bootstrap");

    stream->publish(make_video_frame(1'000'000'000, true));
    stream->publish(make_video_frame(1'040'000'000, false));
    stream->publish(make_video_frame(2'000'000'000, true));
    stream->publish(make_video_frame(2'040'000'000, false));
    stream->publish(make_video_frame(3'000'000'000, true));
    drain();
    require(capture.writes.size() == 2U, "http flv does not request another batch while current write is pending");

    output->write_complete(1);
    drain();
    require(capture.writes.size() == 3U, "http flv keeps consuming prefetched current batch");

    output->write_complete(1);
    drain();
    require(capture.writes.size() == 4U, "http flv requests next batch after current batch completes");
    const auto decoded = demux_http_flv(capture);
    std::vector<std::int64_t> video_pts;
    for (const auto& packet : decoded.packets)
    {
        if (packet.codec == FLV_VIDEO_H264)
        {
            video_pts.push_back(packet.pts);
        }
    }
    require(video_pts == std::vector<std::int64_t>{0, 40, 3'000},
            "http flv retains one bounded batch and resyncs only when requesting the next batch");
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

    auto stream = std::make_shared<media_stream>("live/http-flv-h265", reader_worker.get_executor());
    require(stream->set_tracks({make_h265_track()}), "http flv h265 track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
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

    auto stream = std::make_shared<media_stream>("live/http-flv-fast-slow", reader_worker.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv fast slow track");
    http_flv_capture fast_capture;
    http_flv_capture slow_capture;
    auto fast = std::make_shared<http_flv_output>(
        [&fast_capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
        { fast_capture.writes.push_back(http_flv_write{.generation = generation, .bootstrap = bootstrap, .data = std::move(data)}); },
        [&fast_capture]() { ++fast_capture.ends; });
    auto slow = std::make_shared<http_flv_output>(
        [&slow_capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
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

    auto stream = std::make_shared<media_stream>("live/http-flv-av", reader_worker.get_executor());
    require(stream->set_tracks({make_video_track(), make_audio_track()}), "http flv av tracks");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
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

void test_http_flv_config_reset()
{
    boost::asio::io_context reader_worker(1);
    const auto drain = [&reader_worker]()
    {
        reader_worker.restart();
        while (reader_worker.poll() != 0)
        {
        }
    };

    auto stream = std::make_shared<media_stream>("live/http-flv-reset", reader_worker.get_executor());
    require(stream->set_tracks({make_video_track()}), "http flv reset initial track");
    http_flv_capture capture;
    auto output = std::make_shared<http_flv_output>(
        [&capture](std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap)
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

void test_rtsp_input_establishment_timeout()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/timeout";
    auto pull = std::make_shared<rtsp_input_session>(
        client_io, registry, "relay/timeout", request_url, std::chrono::milliseconds(100));
    const std::weak_ptr<rtsp_input_session> weak_pull = pull;
    require(pull->startup(), "rtsp establishment timeout pull startup");
    pull.reset();

    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);
    const auto describe = read_rtsp_headers(socket);
    require(describe.starts_with("DESCRIBE " + request_url + " RTSP/1.0\r\n"), "rtsp establishment timeout describe");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registry.find("relay/timeout") && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("relay/timeout"), "rtsp establishment timeout removes stream");
    const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak_pull.expired() && std::chrono::steady_clock::now() < release_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(weak_pull.expired(), "rtsp establishment timeout releases session");

    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_establishment_progress_timeout()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/play-timeout";
    auto pull = std::make_shared<rtsp_input_session>(
        client_io, registry, "relay/play-timeout", request_url, std::chrono::milliseconds(800));
    const std::weak_ptr<rtsp_input_session> weak_pull = pull;
    require(pull->startup(), "rtsp establishment progress timeout pull startup");
    pull.reset();

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
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto setup = read_rtsp_headers(socket);
    const auto setup_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(setup, "CSeq:") +
                                "\r\nSession: play-timeout;timeout=60\r\n"
                                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    boost::asio::write(socket, boost::asio::buffer(setup_response));

    const auto play = read_rtsp_headers(socket);
    require(play.starts_with("PLAY "), "rtsp establishment progress timeout play request");
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: play-timeout;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (registry.find("relay/play-timeout") && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("relay/play-timeout"), "rtsp establishment progress timeout removes stream");
    const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak_pull.expired() && std::chrono::steady_clock::now() < release_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(weak_pull.expired(), "rtsp establishment progress timeout releases session");

    boost::system::error_code error;
    socket.close(error);
    runner.join();
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
                     "m=audio 0 RTP/AVP 100 99\r\n"
                     "a=rtpmap:100 MPEG4-GENERIC/44100/2\r\n"
                     "a=fmtp:100 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n"
                     "a=rtpmap:99 opus/48000/2\r\n"
                     "a=fmtp:99 sprop-stereo=1\r\n"
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

void test_rtsp_input_opus_passthrough_case(std::string_view fmtp, std::uint16_t expected_channels)
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto stream_name = "relay/opus-" + std::to_string(expected_channels) + "-" + std::to_string(fmtp.size());
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/opus";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, stream_name, request_url);
    require(pull->startup(), "rtsp opus input startup");
    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);

    const auto describe = read_rtsp_headers(socket);
    auto sdp = std::string("v=0\r\n") +
               "o=- 0 0 IN IP4 127.0.0.1\r\n"
               "s=test\r\n"
               "c=IN IP4 127.0.0.1\r\n"
               "t=0 0\r\n"
               "m=video 0 RTP/AVP 96\r\n"
               "a=rtpmap:96 H264/90000\r\n"
               "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
               "a=control:video\r\n"
               "m=audio 0 RTP/AVP 111\r\n"
               "a=rtpmap:111 OpUs/48000/2\r\n";
    if (!fmtp.empty())
    {
        sdp += "a=fmtp:111 minptime=10;" + std::string(fmtp) + ";useinbandfec=1\r\n";
    }
    sdp += "a=control:audio\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto video_setup = read_rtsp_headers(socket);
    require(video_setup.starts_with("SETUP " + request_url + "/video RTSP/1.0\r\n"), "rtsp opus input video setup");
    const auto video_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(video_setup, "CSeq:") +
                                "\r\nSession: opus-input;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(video_response));

    const auto audio_setup = read_rtsp_headers(socket);
    require(audio_setup.starts_with("SETUP " + request_url + "/audio RTSP/1.0\r\n"), "rtsp opus dynamic payload setup");
    const auto audio_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(audio_setup, "CSeq:") +
                                "\r\nSession: opus-input;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(audio_response));

    const auto play = read_rtsp_headers(socket);
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: opus-input;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    constexpr std::array<std::uint8_t, 21> video_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::write(socket, boost::asio::buffer(video_rtp));

    std::shared_ptr<media_stream> stream;
    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(stream = registry.find(stream_name)) && std::chrono::steady_clock::now() < stream_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(stream != nullptr, "rtsp opus input publishes stream");
    const auto tracks = stream->tracks();
    require(tracks.size() == 2U, "rtsp opus input complete topology");
    require(tracks[1].codec == codec_id::opus && tracks[1].clock_rate == 48'000 && tracks[1].channel_count == expected_channels &&
                tracks[1].codec_config.empty(),
            "rtsp opus input core track contract");

    auto sink = std::make_shared<raw_audio_capture_sink>();
    stream->add_sink(sink);
    const auto sink_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sink->tracks().size() < 2U && std::chrono::steady_clock::now() < sink_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(sink->tracks().size() == 2U, "rtsp opus input sink ready");

    const std::array<std::vector<std::uint8_t>, 3> opus_payloads{
        std::vector<std::uint8_t>{0xf8, 0xff, 0xfe},
        std::vector<std::uint8_t>{0x78, 0x11, 0x22, 0x33},
        std::vector<std::uint8_t>{0x48, 0x44, 0x55},
    };
    for (std::size_t index = 0; index < opus_payloads.size(); ++index)
    {
        std::vector<std::uint8_t> packet(12U + opus_payloads[index].size());
        packet[0] = 0x80;
        packet[1] = 0xef;
        packet[2] = 0;
        packet[3] = static_cast<std::uint8_t>(index + 1U);
        const auto timestamp = static_cast<std::uint32_t>(index * 960U);
        packet[4] = static_cast<std::uint8_t>(timestamp >> 24U);
        packet[5] = static_cast<std::uint8_t>(timestamp >> 16U);
        packet[6] = static_cast<std::uint8_t>(timestamp >> 8U);
        packet[7] = static_cast<std::uint8_t>(timestamp);
        packet[8] = 0x87;
        packet[9] = 0x65;
        packet[10] = 0x43;
        packet[11] = 0x21;
        std::copy(opus_payloads[index].begin(), opus_payloads[index].end(), packet.begin() + 12);
        const std::array<std::uint8_t, 4> header{
            0x24, 0x02, static_cast<std::uint8_t>(packet.size() >> 8U), static_cast<std::uint8_t>(packet.size())};
        boost::asio::write(socket, boost::asio::buffer(header));
        boost::asio::write(socket, boost::asio::buffer(packet));
    }

    const auto frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sink->frames().size() < opus_payloads.size() && std::chrono::steady_clock::now() < frame_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto frames = sink->frames();
    require(frames.size() == opus_payloads.size(), "rtsp opus input frame count");
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        require(*frames[index].payload == opus_payloads[index], "rtsp opus input raw payload");
        require(frames[index].pts_ns == static_cast<std::int64_t>(index) * 20'000'000 && frames[index].dts_ns == frames[index].pts_ns,
                "rtsp opus input 20ms timeline");
    }

    pull->shutdown();
    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_opus_passthrough()
{
    test_rtsp_input_opus_passthrough_case({}, 1);
    test_rtsp_input_opus_passthrough_case("sprop-stereo=0", 1);
    test_rtsp_input_opus_passthrough_case("sprop-stereo=1", 2);
}

void test_rtsp_input_rejects_invalid_opus_rate()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/opus-rate";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/opus-rate", request_url);
    require(pull->startup(), "rtsp invalid opus rate startup");
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
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:video\r\n"
        "m=audio 0 RTP/AVP 111\r\n"
        "a=rtpmap:111 opus/16000/2\r\n"
        "a=control:audio\r\n";
    const auto response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                          "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(response));

    const auto setup = read_rtsp_headers(socket);
    require(setup.starts_with("SETUP " + request_url + "/video RTSP/1.0\r\n"), "rtsp invalid opus rate skips audio");
    const auto setup_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(setup, "CSeq:") +
                                "\r\nSession: opus-rate;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(setup_response));
    const auto play = read_rtsp_headers(socket);
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: opus-rate;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    constexpr std::array<std::uint8_t, 21> video_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::write(socket, boost::asio::buffer(video_rtp));
    std::shared_ptr<media_stream> stream;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(stream = registry.find("relay/opus-rate")) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(stream != nullptr && stream->tracks().size() == 1U && stream->tracks()[0].kind == media_kind::video,
            "rtsp invalid opus rate excluded from topology");

    pull->shutdown();
    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_g711_passthrough_case(codec_id codec, bool explicit_rtpmap)
{
    require(codec == codec_id::g711a || codec == codec_id::g711u, "rtsp g711 input codec");
    const auto payload_type = codec == codec_id::g711a ? RTP_PAYLOAD_PCMA : RTP_PAYLOAD_PCMU;
    const auto encoding = codec == codec_id::g711a ? "PCMA" : "PCMU";

    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto stream_name = "relay/" + std::string(to_string(codec)) + (explicit_rtpmap ? "-rtpmap" : "-static");
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/g711";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, stream_name, request_url);
    require(pull->startup(), "rtsp g711 input startup");
    std::jthread runner([&client_io]() { client_io.run(); });
    boost::asio::ip::tcp::socket socket(server_io);
    acceptor.accept(socket);

    const auto describe = read_rtsp_headers(socket);
    auto sdp = std::string("v=0\r\n") +
               "o=- 0 0 IN IP4 127.0.0.1\r\n"
               "s=test\r\n"
               "c=IN IP4 127.0.0.1\r\n"
               "t=0 0\r\n"
               "m=video 0 RTP/AVP 96\r\n"
               "a=rtpmap:96 H264/90000\r\n"
               "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
               "a=control:video\r\n"
               "m=audio 0 RTP/AVP " +
               std::to_string(payload_type) + "\r\n";
    if (explicit_rtpmap)
    {
        sdp += "a=rtpmap:" + std::to_string(payload_type) + " " + encoding + "/8000\r\n";
    }
    sdp += "a=control:audio\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto video_setup = read_rtsp_headers(socket);
    require(video_setup.starts_with("SETUP " + request_url + "/video RTSP/1.0\r\n"), "rtsp g711 input video setup");
    const auto video_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(video_setup, "CSeq:") +
                                "\r\nSession: g711-input;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(video_response));

    const auto audio_setup = read_rtsp_headers(socket);
    require(audio_setup.starts_with("SETUP " + request_url + "/audio RTSP/1.0\r\n"), "rtsp g711 static payload setup");
    const auto audio_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(audio_setup, "CSeq:") +
                                "\r\nSession: g711-input;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(audio_response));

    const auto play = read_rtsp_headers(socket);
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: g711-input;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    constexpr std::array<std::uint8_t, 21> video_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::write(socket, boost::asio::buffer(video_rtp));

    std::shared_ptr<media_stream> stream;
    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(stream = registry.find(stream_name)) && std::chrono::steady_clock::now() < stream_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(stream != nullptr, "rtsp g711 input publishes stream");
    const auto tracks = stream->tracks();
    require(tracks.size() == 2U && tracks[1].codec == codec && tracks[1].clock_rate == 8'000 && tracks[1].channel_count == 1 &&
                tracks[1].codec_config.empty(),
            "rtsp g711 input core track contract");

    auto sink = std::make_shared<raw_audio_capture_sink>();
    stream->add_sink(sink);
    const auto sink_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sink->tracks().size() < 2U && std::chrono::steady_clock::now() < sink_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(sink->tracks().size() == 2U, "rtsp g711 input sink ready");

    std::array<std::vector<std::uint8_t>, 3> payloads;
    for (std::size_t index = 0; index < payloads.size(); ++index)
    {
        payloads[index].assign(160U, static_cast<std::uint8_t>(0x20U + index));
        std::vector<std::uint8_t> packet(12U + payloads[index].size());
        packet[0] = 0x80;
        packet[1] = static_cast<std::uint8_t>(payload_type);
        packet[2] = 0;
        packet[3] = static_cast<std::uint8_t>(index + 1U);
        const auto timestamp = static_cast<std::uint32_t>(index * 160U);
        packet[4] = static_cast<std::uint8_t>(timestamp >> 24U);
        packet[5] = static_cast<std::uint8_t>(timestamp >> 16U);
        packet[6] = static_cast<std::uint8_t>(timestamp >> 8U);
        packet[7] = static_cast<std::uint8_t>(timestamp);
        packet[8] = 0x76;
        packet[9] = 0x54;
        packet[10] = 0x32;
        packet[11] = 0x10;
        std::copy(payloads[index].begin(), payloads[index].end(), packet.begin() + 12);
        const std::array<std::uint8_t, 4> header{
            0x24, 0x02, static_cast<std::uint8_t>(packet.size() >> 8U), static_cast<std::uint8_t>(packet.size())};
        boost::asio::write(socket, boost::asio::buffer(header));
        boost::asio::write(socket, boost::asio::buffer(packet));
    }

    const auto frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sink->frames().size() < payloads.size() && std::chrono::steady_clock::now() < frame_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto frames = sink->frames();
    require(frames.size() == payloads.size(), "rtsp g711 input frame count");
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        require(*frames[index].payload == payloads[index], "rtsp g711 input raw payload");
        require(frames[index].pts_ns == static_cast<std::int64_t>(index) * 20'000'000 && frames[index].dts_ns == frames[index].pts_ns,
                "rtsp g711 input 20ms timeline");
    }

    pull->shutdown();
    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_g711_passthrough()
{
    test_rtsp_input_g711_passthrough_case(codec_id::g711a, false);
    test_rtsp_input_g711_passthrough_case(codec_id::g711u, false);
    test_rtsp_input_g711_passthrough_case(codec_id::g711a, true);
}

void test_rtsp_input_rejects_mismatched_g711_rtpmap()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/g711-mismatch";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/g711-mismatch", request_url);
    require(pull->startup(), "rtsp mismatched g711 startup");
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
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:video\r\n"
        "m=audio 0 RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMU/8000\r\n"
        "a=control:audio\r\n";
    const auto response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                          "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(response));

    const auto setup = read_rtsp_headers(socket);
    require(setup.starts_with("SETUP " + request_url + "/video RTSP/1.0\r\n"), "rtsp mismatched g711 skips audio");
    const auto setup_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(setup, "CSeq:") +
                                "\r\nSession: g711-mismatch;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(setup_response));
    const auto play = read_rtsp_headers(socket);
    const auto play_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") +
                               "\r\nSession: g711-mismatch;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    pull->shutdown();
    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_rejects_audio_only_source()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/audio-only";
    auto pull = std::make_shared<rtsp_input_session>(client_io, registry, "relay/audio-only", request_url);
    const std::weak_ptr<rtsp_input_session> weak_pull = pull;
    require(pull->startup(), "rtsp audio only pull startup");
    pull.reset();

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
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 MPEG4-GENERIC/44100/2\r\n"
        "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n"
        "a=control:audio\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" +
                                   std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto setup = read_rtsp_headers(socket);
    const auto setup_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(setup, "CSeq:") +
                                "\r\nSession: audio-only;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(setup_response));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak_pull.expired() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(weak_pull.expired(), "rtsp audio only source rejected after setup");
    require(!registry.find("relay/audio-only"), "rtsp audio only source never enters registry");

    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_uses_complete_sdp_topology_without_track_wait()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/topology";
    auto pull = std::make_shared<rtsp_input_session>(client_io,
                                                     registry,
                                                     "relay/topology",
                                                     request_url,
                                                     std::chrono::milliseconds(500),
                                                     std::chrono::milliseconds(50));
    require(pull->startup(), "rtsp topology pull startup");
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
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:video\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 MPEG4-GENERIC/44100/2\r\n"
        "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n"
        "a=control:audio\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" +
                                   std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto video_setup = read_rtsp_headers(socket);
    const auto video_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(video_setup, "CSeq:") +
                                "\r\nSession: topology;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(video_response));

    const auto audio_setup = read_rtsp_headers(socket);
    const auto audio_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(audio_setup, "CSeq:") +
                                "\r\nSession: topology;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(audio_response));

    const auto play = read_rtsp_headers(socket);
    const auto play_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") + "\r\nSession: topology;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    std::vector<std::vector<std::uint8_t>> video_packets;
    const auto collect_rtp = +[](void* param, int, const void* packet, int bytes, std::uint32_t, int)
    {
        auto& packets = *static_cast<std::vector<std::vector<std::uint8_t>>*>(param);
        const auto* begin = static_cast<const std::uint8_t*>(packet);
        packets.emplace_back(begin, begin + bytes);
        return 0;
    };
    auto* video_muxer = rtsp_muxer_create(collect_rtp, &video_packets);
    require(video_muxer != nullptr, "rtsp topology video muxer");
    const auto video_payload = rtsp_muxer_add_payload(
        video_muxer, "RTP/AVP", 90'000, 96, "H264", 0, 0x12345678U, 0, h264_config.data(), static_cast<int>(h264_config.size()));
    require(video_payload >= 0, "rtsp topology video payload");
    const auto video_media = rtsp_muxer_add_media(
        video_muxer, video_payload, RTP_PAYLOAD_H264, h264_config.data(), static_cast<int>(h264_config.size()));
    require(video_media >= 0, "rtsp topology video media");
    const auto video = make_video_frame(0, true);
    require(rtsp_muxer_input(video_muxer, video_media, 0, 0, video.payload->data(), static_cast<int>(video.payload->size()), 1) == 0,
            "rtsp topology video frame");
    require(rtsp_muxer_destroy(video_muxer) == 0 && !video_packets.empty(), "rtsp topology video packets");
    for (const auto& packet : video_packets)
    {
        std::array<std::uint8_t, 4> header{0x24, 0, static_cast<std::uint8_t>(packet.size() >> 8U), static_cast<std::uint8_t>(packet.size())};
        boost::asio::write(socket, boost::asio::buffer(header));
        boost::asio::write(socket, boost::asio::buffer(packet));
    }

    std::shared_ptr<media_stream> stream;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(stream = registry.find("relay/topology")) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(stream != nullptr, "rtsp publishes complete sdp topology on first rtp");
    const auto tracks = stream->tracks();
    require(tracks.size() == 2U && tracks[0].kind == media_kind::video && tracks[1].kind == media_kind::audio,
            "rtsp sdp topology contains audio and video");
    require(tracks[0].codec_config == h264_config && tracks[1].codec_config == aac_asc, "rtsp sdp topology keeps codec config");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(registry.find("relay/topology") != nullptr, "rtsp complete sdp topology does not wait on initial tracks timer");

    pull->shutdown();
    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_initial_tracks_timeout()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/initial-tracks-timeout";
    auto pull = std::make_shared<rtsp_input_session>(client_io,
                                                     registry,
                                                     "relay/initial-tracks-timeout",
                                                     request_url,
                                                     std::chrono::milliseconds(500),
                                                     std::chrono::milliseconds(100));
    const std::weak_ptr<rtsp_input_session> weak_pull = pull;
    require(pull->startup(), "rtsp initial tracks timeout pull startup");
    pull.reset();

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
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:video\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 MPEG4-GENERIC/44100/2\r\n"
        "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3\r\n"
        "a=control:audio\r\n";
    const auto describe_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(describe, "CSeq:") + "\r\nContent-Base: " + request_url +
                                   "/\r\nContent-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" +
                                   std::string(sdp);
    boost::asio::write(socket, boost::asio::buffer(describe_response));

    const auto video_setup = read_rtsp_headers(socket);
    const auto video_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(video_setup, "CSeq:") +
                                "\r\nSession: initial-tracks-timeout;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(video_response));

    const auto audio_setup = read_rtsp_headers(socket);
    const auto audio_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(audio_setup, "CSeq:") +
                                "\r\nSession: initial-tracks-timeout;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(audio_response));

    const auto play = read_rtsp_headers(socket);
    const auto play_response = "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(play, "CSeq:") +
                               "\r\nSession: initial-tracks-timeout;timeout=60\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(play_response));

    std::vector<std::vector<std::uint8_t>> video_packets;
    const auto collect_rtp = +[](void* param, int, const void* packet, int bytes, std::uint32_t, int)
    {
        auto& packets = *static_cast<std::vector<std::vector<std::uint8_t>>*>(param);
        const auto* begin = static_cast<const std::uint8_t*>(packet);
        packets.emplace_back(begin, begin + bytes);
        return 0;
    };
    auto* video_muxer = rtsp_muxer_create(collect_rtp, &video_packets);
    require(video_muxer != nullptr, "rtsp initial tracks timeout video muxer");
    const auto video_payload = rtsp_muxer_add_payload(
        video_muxer, "RTP/AVP", 90'000, 96, "H264", 0, 0x12345678U, 0, h264_config.data(), static_cast<int>(h264_config.size()));
    require(video_payload >= 0, "rtsp initial tracks timeout video payload");
    const auto video_media = rtsp_muxer_add_media(
        video_muxer, video_payload, RTP_PAYLOAD_H264, h264_config.data(), static_cast<int>(h264_config.size()));
    require(video_media >= 0, "rtsp initial tracks timeout video media");
    const auto video = make_video_frame(0, true);
    require(rtsp_muxer_input(video_muxer, video_media, 0, 0, video.payload->data(), static_cast<int>(video.payload->size()), 1) == 0,
            "rtsp initial tracks timeout video frame");
    require(rtsp_muxer_destroy(video_muxer) == 0 && !video_packets.empty(), "rtsp initial tracks timeout video packets");
    for (const auto& packet : video_packets)
    {
        std::array<std::uint8_t, 4> header{0x24, 0, static_cast<std::uint8_t>(packet.size() >> 8U), static_cast<std::uint8_t>(packet.size())};
        boost::asio::write(socket, boost::asio::buffer(header));
        boost::asio::write(socket, boost::asio::buffer(packet));
    }

    require(!registry.find("relay/initial-tracks-timeout"), "rtsp incomplete stream never enters registry");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak_pull.expired() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(weak_pull.expired(), "rtsp initial tracks timeout releases session");
    require(!registry.find("relay/initial-tracks-timeout"), "rtsp initial tracks timeout leaves registry empty");

    boost::system::error_code error;
    socket.close(error);
    runner.join();
}

void test_rtsp_input_independent_keepalive()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::io_context client_io;
    stream_registry registry;
    const auto request_url = "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/keepalive";
    auto pull = std::make_shared<rtsp_input_session>(
        client_io, registry, "relay/keepalive", request_url, std::chrono::milliseconds(750));
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

    constexpr std::array<std::uint8_t, 21> interleaved_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    boost::asio::write(socket, boost::asio::buffer(interleaved_rtp));

    const auto options = read_rtsp_headers_until(socket, std::chrono::seconds(2));
    require(options.starts_with("OPTIONS * RTSP/1.0\r\n"), "rtsp keepalive timer sends options without more media");
    require(options.find("Session: keepalive-session\r\n") != std::string::npos, "rtsp keepalive carries session");
    const auto options_response =
        "RTSP/1.0 200 OK\r\nCSeq: " + rtsp_header_value(options, "CSeq:") + "\r\nPublic: OPTIONS\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(options_response));

    boost::system::error_code error;
    socket.close(error);
    runner.join();
    require(!registry.find("relay/keepalive"), "rtsp keepalive pull closes");
}


void test_rtsp_publish_server_contract()
{
    {
        io_context_pool workers(1);
        boost::asio::ip::tcp::acceptor probe(workers.context(0), {boost::asio::ip::tcp::v4(), 0});
        const auto port = probe.local_endpoint().port();
        probe.close();
        stream_registry registry;
        auto server = std::make_shared<rtsp_server>(workers, registry, port);
        require(!server->startup(), "rtsp publish router server startup");
        std::jthread runner([&workers]() { workers.run(); });

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(), port});
        constexpr std::string_view partial = "ANNOUNCE rtsp://127.0.0.1/live/partial RTSP/1.0\r\nCSeq: 1\r\n";
        boost::asio::write(client, boost::asio::buffer(partial));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        server->shutdown();
        workers.release_work();
        runner.join();
        client.non_blocking(true);
        std::array<std::uint8_t, 1> byte{};
        boost::system::error_code error;
        static_cast<void>(client.read_some(boost::asio::buffer(byte), error));
        require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset || error == boost::asio::error::not_connected,
                "rtsp publish router shutdown closes partial connection");
    }

    io_context_pool workers(1);
    boost::asio::ip::tcp::acceptor probe(workers.context(0), {boost::asio::ip::tcp::v4(), 0});
    const auto port = probe.local_endpoint().port();
    probe.close();
    stream_registry registry;
    auto server = std::make_shared<rtsp_server>(workers, registry, port);
    require(!server->startup(), "rtsp publish server startup");
    std::jthread runner([&workers]() { workers.run(); });

    boost::asio::io_context client_io;
    boost::asio::ip::tcp::socket client(client_io);
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    const auto base = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/publish";
    const auto video_control = base + "/trackID=0";
    const auto request = [&](std::string value)
    {
        boost::asio::write(client, boost::asio::buffer(value));
        return read_rtsp_headers(client);
    };

    const auto options = request("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    require(options.starts_with("RTSP/1.0 200"), "rtsp publish router options");
    require(rtsp_header_value(options, "Public:") == "OPTIONS,DESCRIBE,SETUP,TEARDOWN,PLAY,ANNOUNCE,RECORD,GET_PARAMETER",
            "rtsp publish router advertised methods");

    const auto sdp = std::string("v=0\r\n") +
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=publish\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=application 0 RTP/AVP 110\r\n"
        "a=rtpmap:110 unknown/90000\r\n"
        "a=control:" + base + "/ignored\r\n" +
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:" + video_control + "\r\n";
    const auto announce = request("ANNOUNCE " + base + " RTSP/1.0\r\nCSeq: 2\r\nContent-Type: application/sdp\r\nContent-Length: " +
                                  std::to_string(sdp.size()) + "\r\n\r\n" + sdp);
    require(announce.starts_with("RTSP/1.0 200"), "rtsp publish announce with ignored media");
    require(!registry.find("live/publish"), "rtsp publish remains private after announce");

    const auto setup = request("SETUP " + video_control +
                               " RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n");
    require(setup.starts_with("RTSP/1.0 200"), "rtsp publish setup selected original media");
    require(!registry.find("live/publish"), "rtsp publish remains private after setup");
    auto session = rtsp_header_value(setup, "Session:");
    if (const auto separator = session.find(';'); separator != std::string::npos)
    {
        session.resize(separator);
    }
    require(!session.empty(), "rtsp publish session id");

    const auto record = request("RECORD " + base + " RTSP/1.0\r\nCSeq: 4\r\nSession: " + session + "\r\n\r\n");
    require(record.starts_with("RTSP/1.0 200"), "rtsp publish record");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::shared_ptr<media_stream> stream;
    while (!(stream = registry.find("live/publish")) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(stream != nullptr, "rtsp publish enters registry on record");
    const auto tracks = stream->tracks();
    require(tracks.size() == 1U && tracks.front().codec == codec_id::h264, "rtsp publish complete initial tracks");

    boost::asio::ip::tcp::socket duplicate(client_io);
    duplicate.connect({boost::asio::ip::address_v4::loopback(), port});
    const auto duplicate_request = [&](std::string value)
    {
        boost::asio::write(duplicate, boost::asio::buffer(value));
        return read_rtsp_headers(duplicate);
    };
    require(duplicate_request("ANNOUNCE " + base + " RTSP/1.0\r\nCSeq: 20\r\nContent-Type: application/sdp\r\nContent-Length: " +
                              std::to_string(sdp.size()) + "\r\n\r\n" + sdp)
                .starts_with("RTSP/1.0 200"),
            "rtsp duplicate publisher announce");
    const auto duplicate_setup = duplicate_request("SETUP " + video_control +
                                                   " RTSP/1.0\r\nCSeq: 21\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n");
    require(duplicate_setup.starts_with("RTSP/1.0 200"), "rtsp duplicate publisher setup");
    auto duplicate_session = rtsp_header_value(duplicate_setup, "Session:");
    if (const auto separator = duplicate_session.find(';'); separator != std::string::npos)
    {
        duplicate_session.resize(separator);
    }
    const auto duplicate_record =
        duplicate_request("RECORD " + base + " RTSP/1.0\r\nCSeq: 22\r\nSession: " + duplicate_session + "\r\n\r\n");
    require(duplicate_record.starts_with("RTSP/1.0 453"), "rtsp duplicate publisher loses first publication race");
    require(registry.find("live/publish") == stream, "rtsp duplicate publisher cleanup keeps winner");
    boost::system::error_code duplicate_error;
    duplicate.close(duplicate_error);

    class frame_sink final : public media_sink
    {
       public:
        void on_track(const media_track&) override
        {
            std::scoped_lock lock(mutex_);
            ++tracks_;
            condition_.notify_all();
        }
        void on_frame(const media_frame&) override
        {
            std::scoped_lock lock(mutex_);
            ++frames_;
            condition_.notify_all();
        }
        void on_end() override {}
        bool wait_for_tracks()
        {
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock, std::chrono::seconds(1), [this]() { return tracks_ > 0U; });
        }
        bool wait_for_frames(std::chrono::milliseconds timeout = std::chrono::seconds(1))
        {
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock, timeout, [this]() { return frames_ > 0U; });
        }

       private:
        std::mutex mutex_;
        std::condition_variable condition_;
        std::size_t tracks_{};
        std::size_t frames_{};
    };

    auto sink = std::make_shared<frame_sink>();
    stream->add_sink(sink);
    require(sink->wait_for_tracks(), "rtsp publish sink sees initial track");
    constexpr std::array<std::uint8_t, 21> interleaved_rtp{
        0x24, 0x00, 0x00, 0x11, 0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::write(client, boost::asio::buffer(interleaved_rtp));
    auto next_interleaved_rtp = interleaved_rtp;
    next_interleaved_rtp[7] = 0x02;
    next_interleaved_rtp[10] = 0x0e;
    next_interleaved_rtp[11] = 0x10;
    boost::asio::write(client, boost::asio::buffer(next_interleaved_rtp));
    require(sink->wait_for_frames(), "rtsp publish interleaved rtp reaches media stream");

    boost::system::error_code error;
    client.close(error);
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registry.find("live/publish") && std::chrono::steady_clock::now() < close_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("live/publish"), "rtsp publish disconnect removes generation");

    boost::asio::ip::tcp::socket republish(client_io);
    republish.connect({boost::asio::ip::address_v4::loopback(), port});
    const auto republish_request = [&](std::string value)
    {
        boost::asio::write(republish, boost::asio::buffer(value));
        return read_rtsp_headers(republish);
    };
    require(republish_request("ANNOUNCE " + base + " RTSP/1.0\r\nCSeq: 30\r\nContent-Type: application/sdp\r\nContent-Length: " +
                              std::to_string(sdp.size()) + "\r\n\r\n" + sdp)
                .starts_with("RTSP/1.0 200"),
            "rtsp republish announce");
    const auto republish_setup = republish_request("SETUP " + video_control +
                                                   " RTSP/1.0\r\nCSeq: 31\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n");
    require(republish_setup.starts_with("RTSP/1.0 200"), "rtsp republish setup");
    auto republish_session = rtsp_header_value(republish_setup, "Session:");
    if (const auto separator = republish_session.find(';'); separator != std::string::npos)
    {
        republish_session.resize(separator);
    }
    require(republish_request("RECORD " + base + " RTSP/1.0\r\nCSeq: 32\r\nSession: " + republish_session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp republish record");
    std::shared_ptr<media_stream> republished;
    const auto republish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(republished = registry.find("live/publish")) && std::chrono::steady_clock::now() < republish_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(republished != nullptr && republished != stream, "rtsp republish creates new generation");
    republish.close(error);
    const auto republish_close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registry.find("live/publish") && std::chrono::steady_clock::now() < republish_close_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("live/publish"), "rtsp republish disconnect removes new generation");

    boost::asio::ip::tcp::socket h265_control(client_io);
    h265_control.connect({boost::asio::ip::address_v4::loopback(), port});
    const auto h265_base = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/publish-h265";
    const auto h265_video_control = h265_base + "/trackID=0";
    const auto h265_request = [&](std::string value)
    {
        boost::asio::write(h265_control, boost::asio::buffer(value));
        return read_rtsp_headers(h265_control);
    };
    const auto h265_hvcc = h265_annex_b_to_hvcc(h265_config);
    require(!h265_hvcc.empty(), "rtsp publish h265 hvcc");
    std::array<std::uint8_t, 2048> h265_media_buffer{};
    const auto h265_media_bytes = sdp_h265(h265_media_buffer.data(),
                                           static_cast<int>(h265_media_buffer.size()),
                                           "RTP/AVP",
                                           0,
                                           96,
                                           90'000,
                                           h265_hvcc.data(),
                                           static_cast<int>(h265_hvcc.size()));
    require(h265_media_bytes > 0, "rtsp publish h265 sdp media");
    std::string h265_media;
    for (int index = 0; index < h265_media_bytes; ++index)
    {
        if (h265_media_buffer[static_cast<std::size_t>(index)] == '\n')
        {
            h265_media += "\r\n";
        }
        else
        {
            h265_media.push_back(static_cast<char>(h265_media_buffer[static_cast<std::size_t>(index)]));
        }
    }
    const auto h265_sdp = std::string("v=0\r\n") +
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=publish-h265\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n" +
        h265_media + "a=control:" + h265_video_control + "\r\n";
    require(h265_request("ANNOUNCE " + h265_base + " RTSP/1.0\r\nCSeq: 40\r\nContent-Type: application/sdp\r\nContent-Length: " +
                         std::to_string(h265_sdp.size()) + "\r\n\r\n" + h265_sdp)
                .starts_with("RTSP/1.0 200"),
            "rtsp publish h265 announce");
    const auto h265_setup = h265_request("SETUP " + h265_video_control +
                                         " RTSP/1.0\r\nCSeq: 41\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n");
    require(h265_setup.starts_with("RTSP/1.0 200"), "rtsp publish h265 setup");
    auto h265_session = rtsp_header_value(h265_setup, "Session:");
    if (const auto separator = h265_session.find(';'); separator != std::string::npos)
    {
        h265_session.resize(separator);
    }
    require(h265_request("RECORD " + h265_base + " RTSP/1.0\r\nCSeq: 42\r\nSession: " + h265_session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp publish h265 record");
    std::shared_ptr<media_stream> h265_stream;
    const auto h265_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(h265_stream = registry.find("live/publish-h265")) && std::chrono::steady_clock::now() < h265_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(h265_stream != nullptr && h265_stream->tracks().size() == 1U && h265_stream->tracks().front().codec == codec_id::h265,
            "rtsp publish h265 topology");
    h265_control.close(error);
    const auto h265_close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registry.find("live/publish-h265") && std::chrono::steady_clock::now() < h265_close_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("live/publish-h265"), "rtsp publish h265 disconnect removes generation");

    boost::asio::ip::tcp::socket udp_control(client_io);
    udp_control.connect({boost::asio::ip::address_v4::loopback(), port});
    const auto udp_base = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/publish-udp";
    const auto udp_video_control = udp_base + "/trackID=0";
    const auto udp_request = [&](std::string value)
    {
        boost::asio::write(udp_control, boost::asio::buffer(value));
        return read_rtsp_headers(udp_control);
    };

    const auto udp_sdp = std::string("v=0\r\n") +
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=publish-udp\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:" + udp_video_control + "\r\n";
    const auto udp_announce = udp_request("ANNOUNCE " + udp_base + " RTSP/1.0\r\nCSeq: 10\r\nContent-Type: application/sdp\r\nContent-Length: " +
                                          std::to_string(udp_sdp.size()) + "\r\n\r\n" + udp_sdp);
    require(udp_announce.starts_with("RTSP/1.0 200"), "rtsp publish udp announce");

    boost::asio::ip::udp::socket udp_rtp(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket udp_rtcp(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto client_rtp_port = udp_rtp.local_endpoint().port();
    const auto client_rtcp_port = udp_rtcp.local_endpoint().port();
    const auto udp_setup = udp_request("SETUP " + udp_video_control + " RTSP/1.0\r\nCSeq: 11\r\nTransport: RTP/AVP;unicast;client_port=" +
                                       std::to_string(client_rtp_port) + "-" + std::to_string(client_rtcp_port) + ";mode=record\r\n\r\n");
    require(udp_setup.starts_with("RTSP/1.0 200"), "rtsp publish udp setup");
    auto udp_session = rtsp_header_value(udp_setup, "Session:");
    if (const auto separator = udp_session.find(';'); separator != std::string::npos)
    {
        udp_session.resize(separator);
    }
    const auto transport = rtsp_header_value(udp_setup, "Transport:");
    const auto server_port_marker = transport.find("server_port=");
    require(server_port_marker != std::string::npos, "rtsp publish udp server ports");
    const auto port_begin = server_port_marker + std::string_view("server_port=").size();
    const auto port_end = transport.find('-', port_begin);
    require(port_end != std::string::npos, "rtsp publish udp server rtp port delimiter");
    unsigned int server_rtp_port{};
    const auto [port_pointer, port_error] = std::from_chars(transport.data() + static_cast<std::ptrdiff_t>(port_begin),
                                                           transport.data() + static_cast<std::ptrdiff_t>(port_end),
                                                           server_rtp_port);
    require(port_error == std::errc{} && port_pointer == transport.data() + static_cast<std::ptrdiff_t>(port_end) && server_rtp_port <= 65'535U,
            "rtsp publish udp server rtp port parse");

    const auto udp_record = udp_request("RECORD " + udp_base + " RTSP/1.0\r\nCSeq: 12\r\nSession: " + udp_session + "\r\n\r\n");
    require(udp_record.starts_with("RTSP/1.0 200"), "rtsp publish udp record");
    std::shared_ptr<media_stream> udp_stream;
    const auto udp_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!(udp_stream = registry.find("live/publish-udp")) && std::chrono::steady_clock::now() < udp_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(udp_stream != nullptr, "rtsp publish udp enters registry");
    auto udp_sink = std::make_shared<frame_sink>();
    udp_stream->add_sink(udp_sink);
    require(udp_sink->wait_for_tracks(), "rtsp publish udp sink sees initial track");

    constexpr std::array<std::uint8_t, 17> udp_rtp_packet{
        0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    boost::asio::ip::udp::socket wrong_udp(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    wrong_udp.send_to(boost::asio::buffer(udp_rtp_packet),
                      {boost::asio::ip::address_v4::loopback(), static_cast<std::uint16_t>(server_rtp_port)});
    require(!udp_sink->wait_for_frames(std::chrono::milliseconds(50)), "rtsp publish udp rejects wrong source port");
    udp_rtp.send_to(boost::asio::buffer(udp_rtp_packet),
                    {boost::asio::ip::address_v4::loopback(), static_cast<std::uint16_t>(server_rtp_port)});
    auto next_udp_rtp_packet = udp_rtp_packet;
    next_udp_rtp_packet[3] = 0x02;
    next_udp_rtp_packet[6] = 0x0e;
    next_udp_rtp_packet[7] = 0x10;
    udp_rtp.send_to(boost::asio::buffer(next_udp_rtp_packet),
                    {boost::asio::ip::address_v4::loopback(), static_cast<std::uint16_t>(server_rtp_port)});
    require(udp_sink->wait_for_frames(), "rtsp publish udp accepts negotiated endpoint");

    udp_control.close(error);
    const auto udp_close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registry.find("live/publish-udp") && std::chrono::steady_clock::now() < udp_close_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!registry.find("live/publish-udp"), "rtsp publish udp disconnect removes generation");

    server->shutdown();
    workers.release_work();
    runner.join();
}

class rtsp_output_test_peer final
{
   public:
    explicit rtsp_output_test_peer(std::vector<media_track> tracks = {make_video_track(), make_audio_track()}, output_video_config video = {})
        : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_(io_)
    {
        stream_ = std::make_shared<media_stream>("live/test", io_.get_executor());
        require(stream_->set_tracks(std::move(tracks)), "rtsp output tracks");
        require(registry_.add(stream_), "rtsp output registry add");

        client_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        auto session = std::make_shared<rtsp_output_session>(std::move(connection), registry_, acceptor_.local_endpoint().port(), video);
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

    void end()
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        boost::asio::post(io_,
                          [this, stream = stream_, promise]()
                          {
                              registry_.remove(*stream);
                              stream->end();
                              promise->set_value();
                          });
        require(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "rtsp output stream end");
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

    const std::string parameter_body(70U * 1024U, 'x');
    const auto get_parameter = peer.request("GET_PARAMETER " + base +
                                            " RTSP/1.0\r\n"
                                            "CSeq: 2\r\n"
                                            "Content-Length: " +
                                            std::to_string(parameter_body.size()) + "\r\n\r\n" + parameter_body);
    require(get_parameter.starts_with("RTSP/1.0 200"), "rtsp output fragmented request body");

    const auto pause = peer.request("PAUSE " + base +
                                    " RTSP/1.0\r\n"
                                    "CSeq: 3\r\n\r\n");
    require(pause.starts_with("RTSP/1.0 501"), "rtsp output pause unsupported");

    const auto set_parameter = peer.request("SET_PARAMETER " + base +
                                            " RTSP/1.0\r\n"
                                            "CSeq: 4\r\n"
                                            "Content-Length: 0\r\n\r\n");
    require(set_parameter.starts_with("RTSP/1.0 501"), "rtsp output set parameter unsupported");

    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 5\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp output describe");
    require(describe.find("a=control:trackID=1\r\n") != std::string::npos, "rtsp output video control");
    require(describe.find("a=control:trackID=2\r\n") != std::string::npos, "rtsp output audio control");

    const auto wrong_stream = peer.request("SETUP rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                           "/live/other/trackID=1 RTSP/1.0\r\n"
                                           "CSeq: 6\r\n"
                                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(wrong_stream.starts_with("RTSP/1.0 404"), "rtsp output setup stream identity");

    const auto record_setup = peer.request("SETUP " + base +
                                           "/trackID=1 RTSP/1.0\r\n"
                                           "CSeq: 61\r\n"
                                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n");
    require(record_setup.starts_with("RTSP/1.0 461"), "rtsp output setup record mode unsupported");

    const auto multicast_setup = peer.request("SETUP " + base +
                                              "/trackID=1 RTSP/1.0\r\n"
                                              "CSeq: 62\r\n"
                                              "Transport: RTP/AVP/TCP;multicast;interleaved=0-1;mode=play\r\n\r\n");
    require(multicast_setup.starts_with("RTSP/1.0 461"), "rtsp output setup multicast unsupported");

    const auto video_setup = peer.request("SETUP " + base +
                                          "/trackID=1 RTSP/1.0\r\n"
                                          "CSeq: 7\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(video_setup.starts_with("RTSP/1.0 200"), "rtsp output video setup");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(!session.empty(), "rtsp output session id");

    const auto duplicate_setup = peer.request("SETUP " + base +
                                              "/trackID=1 RTSP/1.0\r\n"
                                              "CSeq: 8\r\n"
                                              "Session: " +
                                              session +
                                              "\r\n"
                                              "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=play\r\n\r\n");
    require(duplicate_setup.starts_with("RTSP/1.0 200"), "rtsp output idempotent setup");

    const auto wrong_session = peer.request("SETUP " + base +
                                            "/trackID=2 RTSP/1.0\r\n"
                                            "CSeq: 9\r\n"
                                            "Session: wrong\r\n"
                                            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(wrong_session.starts_with("RTSP/1.0 454"), "rtsp output setup session identity");

    const auto channel_conflict = peer.request("SETUP " + base +
                                               "/trackID=2 RTSP/1.0\r\n"
                                               "CSeq: 10\r\n"
                                               "Session: " +
                                               session +
                                               "\r\n"
                                               "Transport: RTP/AVP/TCP;unicast;interleaved=1-2\r\n\r\n");
    require(channel_conflict.starts_with("RTSP/1.0 461"), "rtsp output interleaved channel conflict");

    const auto audio_setup = peer.request("SETUP " + base +
                                          "/trackID=2 RTSP/1.0\r\n"
                                          "CSeq: 11\r\n"
                                          "Session: " +
                                          session +
                                          "\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(audio_setup.starts_with("RTSP/1.0 200"), "rtsp output audio setup");

    const auto wrong_play = peer.request("PLAY rtsp://127.0.0.1:" + std::to_string(peer.port()) +
                                         "/live/other RTSP/1.0\r\n"
                                         "CSeq: 12\r\n"
                                         "Session: " +
                                         session + "\r\n\r\n");
    require(wrong_play.starts_with("RTSP/1.0 404"), "rtsp output play stream identity");

    const auto play = peer.request("PLAY " + base +
                                   " RTSP/1.0\r\n"
                                   "CSeq: 13\r\n"
                                   "Session: " +
                                   session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp output play");

    const auto late_setup = peer.request("SETUP " + base +
                                         "/trackID=1 RTSP/1.0\r\n"
                                         "CSeq: 14\r\n"
                                         "Session: " +
                                         session +
                                         "\r\n"
                                         "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(late_setup.starts_with("RTSP/1.0 455"), "rtsp output reject setup after play");

    const auto teardown = peer.request("TEARDOWN " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 15\r\n"
                                       "Session: " +
                                       session + "\r\n\r\n");
    require(teardown.starts_with("RTSP/1.0 200"), "rtsp output teardown");
    for (int attempt = 0; attempt < 100 && peer.session_alive(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!peer.session_alive(), "rtsp output teardown releases session");
}

void test_rtsp_output_pull_media()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp pull describe");

    const auto video_setup = peer.request("SETUP " + base +
                                          "/trackID=1 RTSP/1.0\r\n"
                                          "CSeq: 2\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(video_setup.starts_with("RTSP/1.0 200") && !session.empty(), "rtsp pull video setup");

    peer.publish(make_audio_frame(0));
    require(!peer.read_interleaved(std::chrono::milliseconds(50)).has_value(), "rtsp does not pull before play");

    const auto play = peer.request("PLAY " + base +
                                   " RTSP/1.0\r\n"
                                   "CSeq: 3\r\n"
                                   "Session: " +
                                   session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp pull play");

    peer.publish(make_audio_frame(20'000'000));
    require(!peer.read_interleaved(std::chrono::milliseconds(50)).has_value(), "rtsp skips unsetup audio");

    auto frame = make_video_frame(40'000'000, true);
    auto payload = std::make_shared<std::vector<std::uint8_t>>(4'000U, 0x55);
    (*payload)[0] = 0;
    (*payload)[1] = 0;
    (*payload)[2] = 0;
    (*payload)[3] = 1;
    (*payload)[4] = 0x65;
    frame.payload = std::move(payload);
    peer.publish(std::move(frame));

    std::size_t rtp_packets = 0;
    bool marker = false;
    while (!marker)
    {
        const auto packet = peer.read_interleaved(std::chrono::seconds(1));
        require(packet.has_value(), "rtsp pull interleaved packet");
        if (packet->channel == 1U)
        {
            continue;
        }
        require(packet->channel == 0U, "rtsp pull setup video channel");
        rtp_packet_t decoded{};
        require(rtp_packet_deserialize(&decoded, packet->payload.data(), static_cast<int>(packet->payload.size())) == 0,
                "rtsp pull rtp packet");
        ++rtp_packets;
        marker = decoded.rtp.m != 0;
    }
    require(rtp_packets > 1U, "rtsp one frame produces multiple rtp packets");

    peer.end();
    for (int attempt = 0; attempt < 100 && peer.session_alive(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!peer.session_alive(), "rtsp stream end releases session");
}

void test_rtsp_output_audio_video_order()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    require(peer.request("DESCRIBE " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 1\r\n"
                         "Accept: application/sdp\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp av describe");
    const auto video_setup = peer.request("SETUP " + base +
                                          "/trackID=1 RTSP/1.0\r\n"
                                          "CSeq: 2\r\n"
                                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(video_setup.starts_with("RTSP/1.0 200") && !session.empty(), "rtsp av video setup");
    require(peer.request("SETUP " + base +
                         "/trackID=2 RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Session: " +
                         session +
                         "\r\n"
                         "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp av audio setup");
    require(peer.request("PLAY " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 4\r\n"
                         "Session: " +
                         session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp av play");

    peer.publish(make_video_frame(0, true));
    peer.publish(make_audio_frame(20'000'000));
    peer.publish(make_video_frame(40'000'000, false));
    peer.publish(make_audio_frame(60'000'000));

    std::vector<std::uint8_t> frame_channels;
    while (frame_channels.size() < 4U)
    {
        const auto packet = peer.read_interleaved(std::chrono::seconds(1));
        require(packet.has_value(), "rtsp av interleaved packet");
        if (packet->channel == 1U || packet->channel == 3U)
        {
            continue;
        }
        require(packet->channel == 0U || packet->channel == 2U, "rtsp av rtp channel");
        rtp_packet_t decoded{};
        require(rtp_packet_deserialize(&decoded, packet->payload.data(), static_cast<int>(packet->payload.size())) == 0,
                "rtsp av rtp packet");
        if (decoded.rtp.m != 0)
        {
            frame_channels.push_back(packet->channel);
        }
    }
    require(frame_channels == std::vector<std::uint8_t>{0U, 2U, 0U, 2U}, "rtsp preserves audio video frame order");
}

void test_rtsp_output_h265()
{
    rtsp_output_test_peer peer({make_h265_track(), make_audio_track()});
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

void test_rtsp_output_av1()
{
    for (const auto input_codec : {codec_id::h264, codec_id::h265})
    {
        const auto source = make_video_transcoder_fixture(input_codec);
        auto video = input_codec == codec_id::h264 ? make_video_track() : make_h265_track();
        video.codec_config = source.codec_config;
        rtsp_output_test_peer peer({std::move(video)}, output_video_config{.codec = output_video_codec::av1});
        const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
        const auto describe = peer.request("DESCRIBE " + base +
                                           " RTSP/1.0\r\n"
                                           "CSeq: 1\r\n"
                                           "Accept: application/sdp\r\n\r\n");
        require(describe.starts_with("RTSP/1.0 200"), "rtsp av1 describe");
        require(describe.find("m=video 0 RTP/AVP 96") != std::string::npos, "rtsp av1 dynamic payload type");
        require(describe.find("a=rtpmap:96 AV1/90000") != std::string::npos, "rtsp av1 rtpmap");
        require(describe.find("a=fmtp:96 profile=0;level-idx=13;tier=0") != std::string::npos, "rtsp av1 fmtp");
        require(describe.find(input_codec == codec_id::h264 ? "H264/90000" : "H265/90000") == std::string::npos,
                "rtsp av1 excludes source codec");

        const auto setup = peer.request("SETUP " + base +
                                        "/trackID=1 RTSP/1.0\r\n"
                                        "CSeq: 2\r\n"
                                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
        require(setup.starts_with("RTSP/1.0 200"), "rtsp av1 setup");
        const auto session = rtsp_header_value(setup, "Session:");
        require(!session.empty(), "rtsp av1 session");
        require(peer.request("PLAY " + base +
                             " RTSP/1.0\r\n"
                             "CSeq: 3\r\n"
                             "Session: " +
                             session + "\r\n\r\n")
                    .starts_with("RTSP/1.0 200"),
                "rtsp av1 play");

        struct av1_capture
        {
            std::vector<std::uint8_t> temporal_unit;
        } capture;
        rtp_payload_t handler{
            .alloc = nullptr,
            .free = nullptr,
            .packet = [](void* param, const void* packet, int bytes, std::uint32_t, int)
            {
                if (param == nullptr || packet == nullptr || bytes <= 0)
                {
                    return -1;
                }
                auto& value = *static_cast<av1_capture*>(param);
                value.temporal_unit.assign(static_cast<const std::uint8_t*>(packet), static_cast<const std::uint8_t*>(packet) + bytes);
                return 0;
            },
        };
        const auto decoder = std::unique_ptr<void, decltype(&rtp_payload_decode_destroy)>(
            rtp_payload_decode_create(96, "AV1", &handler, &capture), &rtp_payload_decode_destroy);
        require(decoder != nullptr, "rtsp av1 depacketizer create");

        for (const auto& frame : source.frames)
        {
            peer.publish(frame);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (capture.temporal_unit.empty() && std::chrono::steady_clock::now() < deadline)
        {
            const auto interleaved = peer.read_interleaved(std::chrono::milliseconds(200));
            if (!interleaved)
            {
                continue;
            }
            if (interleaved->channel == 1U)
            {
                continue;
            }
            require(interleaved->channel == 0U, "rtsp av1 rtp channel");
            rtp_packet_t packet{};
            require(rtp_packet_deserialize(&packet, interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) == 0,
                    "rtsp av1 rtp packet");
            require(packet.rtp.pt == 96U, "rtsp av1 rtp payload type");
            require(rtp_payload_decode_input(decoder.get(), interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) >= 0,
                    "rtsp av1 depacketize");
        }
        require(!capture.temporal_unit.empty(), "rtsp av1 temporal unit");
        aom_av1_t av1{};
        require(aom_av1_codec_configuration_record_init(&av1, capture.temporal_unit.data(), capture.temporal_unit.size()) == 0,
                "rtsp av1 sequence header");
        require(av1.seq_profile == 0 && av1.seq_level_idx_0 <= 13 && av1.seq_tier_0 == 0,
                "rtsp av1 stream parameters match sdp");
    }
}

void test_rtsp_output_opus_passthrough_boundaries()
{
    rtsp_output_test_peer peer({make_video_track(), make_opus_track(1)});
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp opus output describe");
    require(describe.find("m=audio ") != std::string::npos && describe.find(" RTP/AVP 97\n") != std::string::npos,
            "rtsp opus output dynamic payload type");
    require(describe.find("a=rtpmap:97 opus/48000/2\n") != std::string::npos, "rtsp opus output rtpmap");

    const auto setup = peer.request("SETUP " + base +
                                    "/trackID=2 RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    const auto session = rtsp_header_value(setup, "Session:");
    require(setup.starts_with("RTSP/1.0 200") && !session.empty(), "rtsp opus output setup");
    require(peer.request("PLAY " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Session: " +
                         session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp opus output play");

    peer.publish(make_video_frame(0, true));
    const auto opus = make_opus_frame(20'000'000, {0x78, 0x11, 0x22, 0x33, 0x44});
    peer.publish(opus);
    std::size_t rtp_packets = 0;
    std::vector<std::uint8_t> received_payload;
    while (const auto interleaved = peer.read_interleaved(std::chrono::milliseconds(100)))
    {
        if (interleaved->channel == 3U)
        {
            continue;
        }
        require(interleaved->channel == 2U, "rtsp opus output rtp channel");
        rtp_packet_t packet{};
        require(rtp_packet_deserialize(&packet, interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) == 0,
                "rtsp opus output rtp packet");
        require(packet.rtp.pt == 97U, "rtsp opus output negotiated payload type");
        const auto* begin = static_cast<const std::uint8_t*>(packet.payload);
        received_payload.assign(begin, begin + packet.payloadlen);
        ++rtp_packets;
    }
    require(rtp_packets == 1U && received_payload == *opus.payload, "rtsp opus output one packet raw payload");

    peer.publish(make_opus_frame(40'000'001));
    require(!peer.read_interleaved(std::chrono::milliseconds(100)).has_value(), "rtsp opus output rejects fractional millisecond");

    const auto capacity = static_cast<std::size_t>(rtp_packet_getsize() - RTP_FIXED_HEADER);
    peer.publish(make_opus_frame(60'000'000, std::vector<std::uint8_t>(capacity + 1U, 0x55)));
    require(!peer.read_interleaved(std::chrono::milliseconds(100)).has_value(), "rtsp opus output rejects oversized packet");
}

void test_rtsp_output_g711_passthrough_case(codec_id codec)
{
    require(codec == codec_id::g711a || codec == codec_id::g711u, "rtsp g711 output codec");
    const auto payload_type = codec == codec_id::g711a ? RTP_PAYLOAD_PCMA : RTP_PAYLOAD_PCMU;
    rtsp_output_test_peer peer({make_video_track(), make_g711_track(codec)});
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request("DESCRIBE " + base +
                                       " RTSP/1.0\r\n"
                                       "CSeq: 1\r\n"
                                       "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp g711 output describe");
    require(describe.find("m=audio 0 RTP/AVP " + std::to_string(payload_type) + "\n") != std::string::npos,
            "rtsp g711 output static payload type");
    require(describe.find("a=rtpmap:" + std::to_string(payload_type)) == std::string::npos,
            "rtsp g711 output does not require rtpmap");

    const auto setup = peer.request("SETUP " + base +
                                    "/trackID=2 RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    const auto session = rtsp_header_value(setup, "Session:");
    require(setup.starts_with("RTSP/1.0 200") && !session.empty(), "rtsp g711 output setup");
    require(peer.request("PLAY " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Session: " +
                         session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp g711 output play");

    peer.publish(make_video_frame(0, true));
    std::array<std::vector<std::uint8_t>, 2> payloads;
    payloads[0].assign(160U, 0x31);
    payloads[1].assign(160U, 0x32);
    peer.publish(make_raw_audio_frame(20'000'000, payloads[0]));
    peer.publish(make_raw_audio_frame(40'000'000, payloads[1]));

    std::vector<rtp_packet_t> packets;
    std::vector<std::vector<std::uint8_t>> received_payloads;
    while (const auto interleaved = peer.read_interleaved(std::chrono::milliseconds(100)))
    {
        if (interleaved->channel == 3U)
        {
            continue;
        }
        require(interleaved->channel == 2U, "rtsp g711 output rtp channel");
        rtp_packet_t packet{};
        require(rtp_packet_deserialize(&packet, interleaved->payload.data(), static_cast<int>(interleaved->payload.size())) == 0,
                "rtsp g711 output rtp packet");
        require(packet.rtp.pt == static_cast<unsigned int>(payload_type), "rtsp g711 output static rtp payload type");
        const auto* begin = static_cast<const std::uint8_t*>(packet.payload);
        received_payloads.emplace_back(begin, begin + packet.payloadlen);
        packets.push_back(packet);
    }
    require(received_payloads == std::vector<std::vector<std::uint8_t>>(payloads.begin(), payloads.end()),
            "rtsp g711 output raw payload");
    require(packets.size() == 2U && packets[1].rtp.timestamp - packets[0].rtp.timestamp == 160U,
            "rtsp g711 output 20ms timestamp step");

    peer.publish(make_raw_audio_frame(60'000'001, std::vector<std::uint8_t>(160U, 0x33)));
    require(!peer.read_interleaved(std::chrono::milliseconds(100)).has_value(), "rtsp g711 output rejects fractional millisecond");

    const auto capacity = static_cast<std::size_t>(rtp_packet_getsize() - RTP_FIXED_HEADER);
    peer.publish(make_raw_audio_frame(80'000'000, std::vector<std::uint8_t>(capacity, 0x44)));
    std::optional<rtsp_interleaved_packet> maximum;
    while (const auto interleaved = peer.read_interleaved(std::chrono::milliseconds(100)))
    {
        if (interleaved->channel == 3U)
        {
            continue;
        }
        maximum = interleaved;
        break;
    }
    require(maximum.has_value() && maximum->channel == 2U, "rtsp g711 output accepts maximum payload");
    rtp_packet_t maximum_packet{};
    require(rtp_packet_deserialize(&maximum_packet, maximum->payload.data(), static_cast<int>(maximum->payload.size())) == 0 &&
                maximum_packet.payloadlen == static_cast<int>(capacity),
            "rtsp g711 output maximum payload size");
    while (const auto interleaved = peer.read_interleaved(std::chrono::milliseconds(20)))
    {
        require(interleaved->channel == 3U, "rtsp g711 output drains rtcp after maximum payload");
    }

    peer.publish(make_raw_audio_frame(100'000'000, std::vector<std::uint8_t>(capacity + 1U, 0x55)));
    require(!peer.read_interleaved(std::chrono::milliseconds(100)).has_value(), "rtsp g711 output rejects oversized packet");
}

void test_rtsp_output_g711_passthrough()
{
    test_rtsp_output_g711_passthrough_case(codec_id::g711a);
    test_rtsp_output_g711_passthrough_case(codec_id::g711u);
}

void test_rtsp_output_setup_track_lifecycle()
{
    {
        rtsp_output_test_peer peer({make_h265_track(), make_audio_track()});
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

void test_rtsp_output_unsetup_audio_update_keeps_video_continuity()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    require(peer.request("DESCRIBE " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 1\r\n"
                         "Accept: application/sdp\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp continuity describe");
    const auto setup = peer.request("SETUP " + base +
                                    "/trackID=1 RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    const auto session = rtsp_header_value(setup, "Session:");
    require(setup.starts_with("RTSP/1.0 200") && !session.empty(), "rtsp continuity video setup");
    require(peer.request("PLAY " + base +
                         " RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Session: " +
                         session + "\r\n\r\n")
                .starts_with("RTSP/1.0 200"),
            "rtsp continuity play");

    peer.publish(make_video_frame(0, true));
    peer.publish(make_video_frame(40'000'000, false));
    std::size_t frames = 0;
    while (frames < 2U)
    {
        const auto packet = peer.read_interleaved(std::chrono::seconds(1));
        require(packet.has_value(), "rtsp continuity initial video packet");
        if (packet->channel != 0U)
        {
            continue;
        }
        rtp_packet_t decoded{};
        require(rtp_packet_deserialize(&decoded, packet->payload.data(), static_cast<int>(packet->payload.size())) == 0,
                "rtsp continuity initial rtp packet");
        frames += decoded.rtp.m != 0 ? 1U : 0U;
    }

    auto audio = make_audio_track();
    audio.clock_rate = 48'000;
    audio.codec_config = {0x11, 0x90};
    require(peer.update_track(std::move(audio)), "rtsp continuity unsetup audio update");
    require(peer.session_alive(), "rtsp continuity session stays alive");

    peer.publish(make_video_frame(80'000'000, false));
    peer.publish(make_video_frame(120'000'000, false));
    while (frames < 4U)
    {
        const auto packet = peer.read_interleaved(std::chrono::milliseconds(200));
        require(packet.has_value(), "rtsp continuity delta after unsetup audio update");
        if (packet->channel != 0U)
        {
            continue;
        }
        rtp_packet_t decoded{};
        require(rtp_packet_deserialize(&decoded, packet->payload.data(), static_cast<int>(packet->payload.size())) == 0,
                "rtsp continuity updated rtp packet");
        frames += decoded.rtp.m != 0 ? 1U : 0U;
    }
    require(peer.session_alive(), "rtsp continuity session remains alive");
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

void test_rtmp_legacy_fourcc_connect_parse()
{
    const auto make_connect = [](std::span<const std::string_view> fourccs) {
        std::array<std::uint8_t, 512> data{};
        auto* current = AMFWriteString(data.data(), data.data() + data.size(), "connect", 7);
        current = AMFWriteDouble(current, data.data() + data.size(), 1.0);
        current = AMFWriteObject(current, data.data() + data.size());
        current = AMFWriteNamedString(current, data.data() + data.size(), "app", 3, "live", 4);
        current = AMFWriteNamed(current, data.data() + data.size(), "fourCcList", 10);
        require(current != nullptr && current + 5 <= data.data() + data.size(), "rtmp fourcc array header fits");
        *current++ = AMF_STRICT_ARRAY;
        const auto count = static_cast<std::uint32_t>(fourccs.size());
        *current++ = static_cast<std::uint8_t>(count >> 24U);
        *current++ = static_cast<std::uint8_t>(count >> 16U);
        *current++ = static_cast<std::uint8_t>(count >> 8U);
        *current++ = static_cast<std::uint8_t>(count);
        for (const auto fourcc : fourccs)
        {
            current = AMFWriteString(current, data.data() + data.size(), fourcc.data(), fourcc.size());
            require(current != nullptr, "rtmp fourcc array value fits");
        }
        current = AMFWriteObjectEnd(current, data.data() + data.size());
        require(current != nullptr, "rtmp fourcc connect object fits");
        return std::pair{data, static_cast<std::size_t>(current - data.data())};
    };

    const auto parse_connect = [](const std::array<std::uint8_t, 512>& data, std::size_t bytes) {
        rtmp_connect_t connect{};
        rtmp_t rtmp{};
        rtmp.param = &connect;
        rtmp.server.onconnect = [](void* param, int result, double transaction, const rtmp_connect_t* parsed) {
            if (result != 0 || transaction != 1.0 || parsed == nullptr)
            {
                return -1;
            }
            *static_cast<rtmp_connect_t*>(param) = *parsed;
            return 0;
        };
        rtmp_chunk_header_t header{};
        header.length = static_cast<std::uint32_t>(bytes);
        require(rtmp_invoke_handler(&rtmp, &header, data.data()) == 0, "rtmp fourcc connect parses");
        return connect;
    };

    constexpr std::array<std::string_view, 3> codecs{"hvc1", "av01", "vp09"};
    const auto [connect_data, connect_bytes] = make_connect(codecs);
    const auto connect = parse_connect(connect_data, connect_bytes);
    require(std::string_view(connect.fourCcList[0]) == "hvc1", "rtmp fourcc preserves hvc1");
    require(std::string_view(connect.fourCcList[1]) == "av01", "rtmp fourcc preserves av01");
    require(std::string_view(connect.fourCcList[2]) == "vp09", "rtmp fourcc preserves vp09");

    constexpr std::array<std::string_view, 1> wildcard{"*"};
    const auto [wildcard_data, wildcard_bytes] = make_connect(wildcard);
    const auto wildcard_connect = parse_connect(wildcard_data, wildcard_bytes);
    require(std::string_view(wildcard_connect.fourCcList[0]) == "*", "rtmp fourcc preserves wildcard");
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

    rtmp_timestamp_state mixed_state;
    require(unwrap_rtmp_timestamp(near_wrap, mixed_state) == static_cast<std::int64_t>(near_wrap),
            "rtmp mixed timeline video before wrap");
    require(unwrap_rtmp_timestamp(5U, mixed_state) == static_cast<std::int64_t>(near_wrap) + 15,
            "rtmp mixed timeline audio after wrap");
    require(unwrap_rtmp_timestamp(std::numeric_limits<std::uint32_t>::max() - 4U, mixed_state) ==
                static_cast<std::int64_t>(near_wrap) + 5,
            "rtmp mixed timeline keeps cross track dts regression");
    require(unwrap_rtmp_timestamp(25U, mixed_state) == static_cast<std::int64_t>(near_wrap) + 35,
            "rtmp mixed timeline continues after cross track wrap");
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

void test_ireader_h266_avpacket_codec_identity()
{
    struct capture
    {
        AVPACKET_CODEC_ID codec{AVCODEC_NONE};
    } captured;

    const auto on_packet = [](void* param, avpacket_t* packet) -> int {
        if (packet != nullptr && packet->stream != nullptr)
        {
            static_cast<capture*>(param)->codec = packet->stream->codecid;
        }
        return 0;
    };

    constexpr std::array<std::uint8_t, 23> config{
        0xfe, 0x03, 0x8e, 0x00, 0x01, 0x00, 0x02, 0x00, 0x70, 0x8f, 0x00, 0x01,
        0x00, 0x02, 0x00, 0x78, 0x90, 0x00, 0x01, 0x00, 0x02, 0x00, 0x80,
    };
    constexpr std::array<std::uint8_t, 8> frame{0x00, 0x00, 0x00, 0x01, 0x00, 0x38, 0x80, 0x00};

    auto* bitstream = avpbs_find(AVCODEC_VIDEO_H266);
    require(bitstream != nullptr, "ireader h266 bitstream helper");

    void* context = bitstream->create(
        7,
        AVCODEC_VIDEO_H266,
        config.data(),
        static_cast<int>(config.size()),
        on_packet,
        &captured);
    require(context != nullptr, "ireader h266 bitstream create");
    require(
        bitstream->input(
            context,
            1,
            1,
            frame.data(),
            static_cast<int>(frame.size()),
            AVPACKET_FLAG_KEY) == 0,
        "ireader h266 bitstream input");
    require(captured.codec == AVCODEC_VIDEO_H266, "ireader h266 codec identity");
    require(bitstream->destroy(&context) == 0, "ireader h266 bitstream destroy");
}

void test_ireader_avs3_flv_mux_codec_identity()
{
    struct capture
    {
        std::vector<std::uint8_t> first_video_tag;
    } captured;

    const auto on_flv = [](void* param, int type, const void* data, std::size_t bytes, std::uint32_t) -> int {
        auto& output = *static_cast<capture*>(param);
        if (type == FLV_TYPE_VIDEO && output.first_video_tag.empty())
        {
            const auto* begin = static_cast<const std::uint8_t*>(data);
            output.first_video_tag.assign(begin, begin + bytes);
        }
        return 0;
    };

    constexpr std::array<std::uint8_t, 12> frame{
        0x00, 0x00, 0x01, 0xb0, 0x20, 0x44, 0x88, 0xf0, 0x00, 0x00, 0x01, 0xb3,
    };

    auto* muxer = flv_muxer_create(on_flv, &captured);
    require(muxer != nullptr, "ireader avs3 flv muxer create");
    require(flv_muxer_avs3(muxer, frame.data(), frame.size(), 0, 0) == 0, "ireader avs3 flv mux");
    flv_muxer_destroy(muxer);

    require(!captured.first_video_tag.empty(), "ireader avs3 sequence tag");
    flv_video_tag_header_t header{};
    require(
        flv_video_tag_header_read(&header, captured.first_video_tag.data(), captured.first_video_tag.size()) == 5,
        "ireader avs3 sequence tag header");
    require(header.codecid == FLV_VIDEO_AVS3, "ireader avs3 codec identity");
    require(header.avpacket == FLV_SEQUENCE_HEADER, "ireader avs3 sequence header type");
}

void test_ireader_rejects_unknown_enhanced_audio_fourcc()
{
    constexpr std::array<std::uint8_t, 5> tag{
        static_cast<std::uint8_t>(FLV_AUDIO_FOURCC | FLV_AVPACKET), 'x', 'x', 'x', 'x',
    };
    flv_audio_tag_header_t header{};
    require(flv_audio_tag_header_read(&header, tag.data(), tag.size()) < 0, "ireader unknown enhanced audio fourcc");
}

void test_ireader_opus_flv_correctness()
{
    const std::vector<std::uint8_t> head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0};
    std::vector<std::vector<std::uint8_t>> tags;
    auto* muxer = flv_muxer_create(
        [](void* param, int type, const void* data, std::size_t bytes, std::uint32_t) {
            if (type == FLV_TYPE_AUDIO)
            {
                const auto* begin = static_cast<const std::uint8_t*>(data);
                static_cast<std::vector<std::vector<std::uint8_t>>*>(param)->emplace_back(begin, begin + bytes);
            }
            return 0;
        },
        &tags);
    require(muxer != nullptr, "ireader opus flv muxer create");
    require(flv_muxer_opus(muxer, head.data(), head.size(), 0, 0) == 0 && tags.size() == 1U, "ireader opus head only sequence tag");
    flv_audio_tag_header_t header{};
    require(flv_audio_tag_header_read(&header, tags.front().data(), tags.front().size()) == 5 && header.avpacket == FLV_SEQUENCE_HEADER,
            "ireader opus sequence header type");
    const std::vector<std::uint8_t> raw{0xf8, 0xff, 0xfe};
    require(flv_muxer_opus(muxer, raw.data(), raw.size(), 20, 20) == 0 && tags.size() == 2U, "ireader opus raw tag");
    require(flv_audio_tag_header_read(&header, tags.back().data(), tags.back().size()) == 5 && header.avpacket == FLV_AVPACKET &&
                std::ranges::equal(std::span<const std::uint8_t>(tags.back()).subspan(5), raw),
            "ireader opus raw payload unchanged");

    auto updated_head = head;
    updated_head[9] = 1;
    require(flv_muxer_opus(muxer, updated_head.data(), updated_head.size(), 40, 40) == 0 && tags.size() == 3U,
            "ireader opus updated head sequence tag");
    require(flv_audio_tag_header_read(&header, tags.back().data(), tags.back().size()) == 5 && header.avpacket == FLV_SEQUENCE_HEADER,
            "ireader opus updated sequence header type");
    opus_head_t parsed_head{};
    require(opus_head_load(tags.back().data() + 5, tags.back().size() - 5, &parsed_head) > 0 && parsed_head.channels == 1,
            "ireader opus updated sequence header content");
    require(flv_muxer_opus(muxer, raw.data(), raw.size(), 60, 60) == 0 && tags.size() == 4U, "ireader opus raw tag after update");
    require(flv_audio_tag_header_read(&header, tags.back().data(), tags.back().size()) == 5 && header.avpacket == FLV_AVPACKET &&
                std::ranges::equal(std::span<const std::uint8_t>(tags.back()).subspan(5), raw),
            "ireader opus raw payload unchanged after update");
    flv_muxer_destroy(muxer);

    flv_demux_capture capture;
    const auto demuxer = std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(
        flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    require(flv_demuxer_input(demuxer.get(), FLV_TYPE_AUDIO, tags.front().data(), tags.front().size(), 0) == 0,
            "ireader valid opus head demux");
    auto invalid = tags.front();
    invalid.resize(6);
    require(flv_demuxer_input(demuxer.get(), FLV_TYPE_AUDIO, invalid.data(), invalid.size(), 0) < 0,
            "ireader invalid opus head rejected");
}

void test_ireader_avpbs_opus_lifetime()
{
    const auto on_packet = [](void* param, avpacket_t*) -> int {
        ++*static_cast<int*>(param);
        return 0;
    };
    int packets{};
    auto* bitstream = avpbs_find(AVCODEC_AUDIO_OPUS);
    require(bitstream != nullptr, "ireader opus bitstream helper");
    void* context = bitstream->create(2, AVCODEC_AUDIO_OPUS, nullptr, 0, on_packet, &packets);
    require(context != nullptr, "ireader opus bitstream create");

    opus_head_t head{};
    head.version = 1;
    head.channels = 2;
    head.input_sample_rate = 48'000;
    std::array<std::uint8_t, 64> encoded{};
    const auto bytes = opus_head_save(&head, encoded.data(), encoded.size());
    require(bytes > 0, "ireader opus head encode");
    require(bitstream->input(context, 0, 0, encoded.data(), bytes, 0) == 0 && packets == 0,
            "ireader opus head only does not allocate media packet");

    const std::array<std::uint8_t, 9> malformed{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 0xff};
    require(bitstream->input(context, 0, 0, malformed.data(), malformed.size(), 0) < 0 && packets == 0,
            "ireader malformed opus head rejected without packet");
    require(bitstream->destroy(&context) == 0 && context == nullptr, "ireader opus bitstream destroy");
}

void test_ireader_av1_packet_lifetime()
{
    struct capture
    {
        int allocations{};
        int frees{};
        int packets{};
    } state;

    rtp_payload_t handler{};
    handler.alloc = [](void* param, int bytes) -> void* {
        auto& capture_state = *static_cast<capture*>(param);
        ++capture_state.allocations;
        return std::malloc(static_cast<std::size_t>(bytes));
    };
    handler.free = [](void* param, void* packet) {
        auto& capture_state = *static_cast<capture*>(param);
        ++capture_state.frees;
        std::free(packet);
    };
    handler.packet = [](void* param, const void*, int, std::uint32_t, int) -> int {
        ++static_cast<capture*>(param)->packets;
        return 0;
    };

    void* encoder = rtp_payload_encode_create(RTP_PAYLOAD_AV1, "AV1", 1, 2, &handler, &state);
    require(encoder != nullptr, "ireader av1 packet encoder create");

    const std::array<std::uint8_t, 5> malformed_after_alloc{0x0a, 0x01, 0x00, 0x0a, 0x7f};
    require(rtp_payload_encode_input(encoder, malformed_after_alloc.data(), malformed_after_alloc.size(), 90'000) < 0,
            "ireader av1 malformed packet rejected");
    require(state.allocations == 1 && state.frees == 0, "ireader av1 malformed packet retains pending buffer for cleanup");

    const std::array<std::uint8_t, 3> valid{0x0a, 0x01, 0x00};
    require(rtp_payload_encode_input(encoder, valid.data(), valid.size(), 93'600) == 0, "ireader av1 packet recovers after malformed input");
    require(state.packets == 1 && state.frees == 2, "ireader av1 stale packet released before next input");
    rtp_payload_encode_destroy(encoder);
    require(state.frees == 2, "ireader av1 destroy does not double free");

    state = {};
    encoder = rtp_payload_encode_create(RTP_PAYLOAD_AV1, "AV1", 1, 2, &handler, &state);
    require(encoder != nullptr, "ireader av1 immediate destroy encoder create");
    require(rtp_payload_encode_input(encoder, malformed_after_alloc.data(), malformed_after_alloc.size(), 97'200) < 0,
            "ireader av1 immediate destroy malformed packet rejected");
    require(state.allocations == 1 && state.frees == 0, "ireader av1 immediate destroy keeps pending buffer");
    rtp_payload_encode_destroy(encoder);
    require(state.frees == 1, "ireader av1 immediate destroy releases pending buffer");
}

void test_ireader_av1_marker_boundary()
{
    struct capture
    {
        std::vector<bool> markers;
    } state;

    rtp_payload_t handler{};
    handler.alloc = [](void*, int bytes) -> void* { return std::malloc(static_cast<std::size_t>(bytes)); };
    handler.free = [](void*, void* packet) { std::free(packet); };
    handler.packet = [](void* param, const void* packet, int bytes, std::uint32_t, int) -> int {
        auto& capture_state = *static_cast<capture*>(param);
        const auto* data = static_cast<const std::uint8_t*>(packet);
        capture_state.markers.push_back(bytes >= 2 && (data[1] & 0x80U) != 0);
        return 0;
    };

    const auto encode = [&](const std::vector<std::uint8_t>& temporal_unit) {
        state.markers.clear();
        void* encoder = rtp_payload_encode_create(RTP_PAYLOAD_AV1, "AV1", 1, 2, &handler, &state);
        require(encoder != nullptr, "ireader av1 marker encoder create");
        require(rtp_payload_encode_input(encoder, temporal_unit.data(), static_cast<int>(temporal_unit.size()), 90'000) == 0,
                "ireader av1 marker packetize");
        rtp_payload_encode_destroy(encoder);
    };

    // 1200 字节 RTP 包下，最后一个 OBU 分片完成后只剩 7 字节空间。
    std::vector<std::uint8_t> fragmented_obu(2'363, 0);
    fragmented_obu[0] = 0x32;
    fragmented_obu[1] = 0xb8;
    fragmented_obu[2] = 0x12;
    encode(fragmented_obu);
    require(state.markers == std::vector<bool>{false, true}, "ireader av1 fragmented temporal unit final marker");

    // 第一个 OBU 完成后仅剩 1 字节，写后续 OBU 前必须先发送当前 RTP 包。
    std::vector<std::uint8_t> aggregated_obus(1'187, 0);
    aggregated_obus[0] = 0x32;
    aggregated_obus[1] = 0x9d;
    aggregated_obus[2] = 0x09;
    aggregated_obus[1'184] = 0x32;
    aggregated_obus[1'185] = 0x01;
    aggregated_obus[1'186] = 0x00;
    encode(aggregated_obus);
    require(state.markers == std::vector<bool>{false, true}, "ireader av1 next obu flush keeps final marker");
}

void test_flv_config_cache_lifecycle()
{
    flv_demux_capture capture;
    const auto demuxer =
        std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    require(demuxer != nullptr, "flv config demuxer create");
    std::size_t video_sequence_headers = 0;
    std::size_t audio_sequence_headers = 0;
    std::vector<std::uint32_t> video_sequence_header_timestamps;
    std::optional<std::int32_t> video_composition_time;
    std::optional<std::uint32_t> video_timestamp;
    flv_output_muxer output(
        [&capture, &demuxer, &video_sequence_headers, &audio_sequence_headers, &video_sequence_header_timestamps, &video_composition_time, &video_timestamp](
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
                video_sequence_header_timestamps.push_back(timestamp);
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                ++audio_sequence_headers;
            }
        });

    auto video = make_video_track();
    video.config_version = 1;
    output.on_track(video);
    require(video_sequence_headers == 1U && video_sequence_header_timestamps == std::vector<std::uint32_t>{0U},
            "flv initial video sequence header timestamp");
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
    require(video_sequence_headers == 1U, "flv config reset defers video sequence header");

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
    require(video_sequence_headers == 2U && video_sequence_header_timestamps.back() == 1'000U,
            "flv audio config reset reprimes video header at next media dts");

    auto updated_video = video;
    updated_video.codec_config = h264_config_updated;
    updated_video.config_version = 2;
    output.on_track(updated_video);
    require(video_sequence_headers == 2U, "flv video config reset defers sequence header");
    output.on_frame(make_video_frame(2'000'000'000, true, h264_config_updated));
    require(video_sequence_headers == 3U && video_sequence_header_timestamps.back() == 2'000U,
            "flv video config reset reprimes header at next media dts");
    const auto avcc_count = std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    require(avcc_count == 3, "flv h264 config generations");
    const auto first_avcc = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    const auto last_avcc = std::ranges::find_if(
        capture.packets.rbegin(), capture.packets.rend(), [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AVCC; });
    require(first_avcc != capture.packets.end() && h264_avcc_to_annex_b(first_avcc->payload) == h264_config, "flv initial h264 config content");
    require(last_avcc != capture.packets.rend() && h264_avcc_to_annex_b(last_avcc->payload) == h264_config_updated,
            "flv updated h264 config content");
}

void test_flv_av1_transcode_round_trip()
{
    for (const auto codec : {codec_id::h264, codec_id::h265})
    {
        const auto fixture = make_video_transcoder_fixture(codec);
        flv_demux_capture capture;
        const auto demuxer = std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(
            flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
        require(demuxer != nullptr, "flv av1 demuxer create");
        flv_output_muxer output(
            [&demuxer](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
            {
                require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv av1 demux input");
            },
            output_video_config{
                .codec = output_video_codec::av1,
            });
        output.on_track(media_track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = codec,
            .clock_rate = 90'000,
            .codec_config = fixture.codec_config,
            .config_version = 1,
        });
        auto malformed = fixture.frames.front();
        malformed.payload = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{0x01, 0x02, 0x03, 0x04});
        output.on_frame(malformed);
        if (codec == codec_id::h264)
        {
            auto audio = make_audio_track();
            audio.config_version = 1;
            output.on_track(audio);
            output.on_frame(make_audio_frame(fixture.frames.front().pts_ns + 10'000'000));
            for (std::size_t index = 0; index + 1U < fixture.frames.size(); ++index)
            {
                output.on_frame(fixture.frames[index]);
            }
            require(std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AV1C; }) == 1,
                    "flv av1 initial configuration before audio update");

            const std::vector<std::uint8_t> updated_asc{0x11, 0x90};
            auto updated_audio = audio;
            updated_audio.clock_rate = 48'000;
            updated_audio.codec_config = updated_asc;
            updated_audio.config_version = 2;
            output.on_track(updated_audio);
            const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
            auto updated_adts = make_adts_frame(updated_asc, raw);
            require(!updated_adts.empty(), "flv av1 updated aac adts");
            output.on_frame(media_frame{
                .track = audio_track_id,
                .dts_ns = fixture.frames.back().dts_ns - 10'000'000,
                .pts_ns = fixture.frames.back().pts_ns - 10'000'000,
                .key_frame = false,
                .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(updated_adts)),
            });
            output.on_frame(fixture.frames.back());
            require(std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AV1C; }) == 1,
                    "flv av1 audio update keeps video configuration");
            require(std::ranges::count_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_AUDIO_ASC; }) == 2,
                    "flv av1 audio update refreshes aac configuration");
        }
        else
        {
            for (const auto& frame : fixture.frames)
            {
                output.on_frame(frame);
            }
        }

        require(std::ranges::none_of(capture.packets, [](const demuxed_packet& packet) {
                    return packet.codec == FLV_VIDEO_AVCC || packet.codec == FLV_VIDEO_HVCC || packet.codec == FLV_VIDEO_H264 ||
                        packet.codec == FLV_VIDEO_H265;
                }),
                "flv av1 generation excludes source video codec");
        const auto config = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AV1C; });
        require(config != capture.packets.end() && !config->payload.empty(), "flv av1 configuration record");
        const auto first_frame = std::ranges::find_if(capture.packets, [](const demuxed_packet& packet) { return packet.codec == FLV_VIDEO_AV1; });
        require(first_frame != capture.packets.end() && !first_frame->payload.empty() && (first_frame->flags & 1) != 0,
                "flv av1 keyframe identity");
        aom_av1_t av1{};
        require(aom_av1_codec_configuration_record_load(config->payload.data(), config->payload.size(), &av1) ==
                        static_cast<int>(config->payload.size()) &&
                    av1.bytes > 0,
                "flv av1 configuration record");
        aom_av1_t sequence{};
        require(aom_av1_codec_configuration_record_init(&sequence, av1.data, av1.bytes) == 0 && sequence.width > 0 && sequence.height > 0 &&
                    sequence.seq_profile == av1.seq_profile && sequence.seq_level_idx_0 == av1.seq_level_idx_0 &&
                    sequence.seq_tier_0 == av1.seq_tier_0,
                "flv av1 configuration record parses stream properties");
    }
}

void test_flv_g711_round_trip()
{
    for (const auto codec : {codec_id::g711a, codec_id::g711u})
    {
        flv_demux_capture capture;
        const auto demuxer = std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(
            flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
        require(demuxer != nullptr, "flv g711 demuxer create");
        flv_output_muxer output([&demuxer](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp) {
            require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv g711 demux input");
        });
        output.on_track(make_g711_track(codec));
        const std::vector<std::uint8_t> payload(160, codec == codec_id::g711a ? 0xd5 : 0xff);
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = 40'000'000,
            .pts_ns = 40'000'000,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(payload),
        });
        require(capture.packets.size() == 1U, "flv g711 packet count");
        require(capture.packets.front().codec == (codec == codec_id::g711a ? FLV_AUDIO_G711A : FLV_AUDIO_G711U),
                "flv g711 codec identity");
        require(capture.packets.front().pts == 40 && capture.packets.front().dts == 40 && capture.packets.front().payload == payload,
                "flv g711 raw payload timestamp");
    }
}

void test_flv_opus_adapter_round_trip()
{
    flv_demux_capture capture;
    const auto demuxer = std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(
        flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    flv_output_muxer output([&demuxer](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp) {
        require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv opus adapter demux");
    });
    output.on_track(make_opus_track(2));
    const std::vector<std::uint8_t> payload{0xf8, 0xff, 0xfe};
    output.on_frame(make_opus_frame(20'000'000, payload));
    require(capture.packets.size() == 2U && capture.packets[0].codec == FLV_AUDIO_OPUS_HEAD &&
                capture.packets[1].codec == FLV_AUDIO_OPUS && capture.packets[1].payload == payload && capture.packets[1].pts == 20,
            "flv opus sequence then raw packet");

    flv_demux_capture av1_capture;
    const auto av1_demuxer = std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(
        flv_demuxer_create(&capture_flv_packet, &av1_capture), &flv_demuxer_destroy);
    flv_output_muxer av1_output(
        [&av1_demuxer](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp) {
            require(flv_demuxer_input(av1_demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv av1 opus adapter demux");
        },
        output_video_config{
            .codec = output_video_codec::av1,
        });
    auto video = make_video_track();
    video.config_version = 1;
    av1_output.on_track(video);
    auto mono = make_opus_track(1);
    mono.config_version = 1;
    av1_output.on_track(mono);
    av1_output.on_frame(make_opus_frame(20'000'000, payload));
    auto stereo = make_opus_track(2);
    stereo.config_version = 2;
    av1_output.on_track(stereo);
    av1_output.on_frame(make_opus_frame(40'000'000, payload));

    require(av1_capture.packets.size() == 4U && av1_capture.packets[0].codec == FLV_AUDIO_OPUS_HEAD &&
                av1_capture.packets[1].codec == FLV_AUDIO_OPUS && av1_capture.packets[2].codec == FLV_AUDIO_OPUS_HEAD &&
                av1_capture.packets[3].codec == FLV_AUDIO_OPUS,
            "flv av1 opus config generation packet order");
    opus_head_t first_head{};
    opus_head_t second_head{};
    require(opus_head_load(av1_capture.packets[0].payload.data(), av1_capture.packets[0].payload.size(), &first_head) > 0 &&
                opus_head_load(av1_capture.packets[2].payload.data(), av1_capture.packets[2].payload.size(), &second_head) > 0 &&
                first_head.channels == 1 && second_head.channels == 2,
            "flv av1 opus config generation channels");
}

void test_h265_output_paths()
{
    flv_demux_capture capture;
    const auto demuxer =
        std::unique_ptr<flv_demuxer_t, decltype(&flv_demuxer_destroy)>(flv_demuxer_create(&capture_flv_packet, &capture), &flv_demuxer_destroy);
    require(demuxer != nullptr, "flv h265 demuxer create");
    std::vector<std::uint32_t> hevc_sequence_header_timestamps;
    flv_output_muxer flv([&demuxer, &hevc_sequence_header_timestamps](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
                         {
                             require(flv_demuxer_input(demuxer.get(), type, data.data(), data.size(), timestamp) == 0, "flv h265 demuxer input");
                             if (type == FLV_TYPE_VIDEO && data.size() >= 2U && (data[0] & 0x0fU) == FLV_VIDEO_H265 && data[1] == 0U)
                             {
                                 hevc_sequence_header_timestamps.push_back(timestamp);
                             }
                         });
    auto hevc_track = make_h265_track();
    hevc_track.config_version = 1;
    flv.on_track(hevc_track);
    require(hevc_sequence_header_timestamps == std::vector<std::uint32_t>{0U}, "flv initial h265 sequence header timestamp");
    const auto hevc_frame = make_h265_frame(40'000'000, true);
    flv.on_frame(hevc_frame);
    auto updated_hevc = hevc_track;
    updated_hevc.codec_config = h265_config_updated;
    updated_hevc.config_version = 2;
    flv.on_track(updated_hevc);
    require(hevc_sequence_header_timestamps.size() == 1U, "flv h265 config reset defers sequence header");
    flv.on_frame(make_h265_frame(1'000'000'000, true, h265_config_updated));
    require(hevc_sequence_header_timestamps == std::vector<std::uint32_t>{0U, 1'000U},
            "flv h265 config reset reprimes header at next media dts");

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

    hls_output hls(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
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
            .video_mid = "0",
            .video_mid_extension_id = 4,
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

void test_ireader_negotiated_payload_mapping()
{
    struct capture
    {
        std::vector<std::uint8_t> packet;
    } state;

    const auto on_packet = [](void* param, int, const void* packet, int bytes, std::uint32_t, int) {
        auto& capture_state = *static_cast<capture*>(param);
        capture_state.packet.assign(static_cast<const std::uint8_t*>(packet), static_cast<const std::uint8_t*>(packet) + bytes);
        return 0;
    };
    auto* muxer = rtsp_muxer_create(on_packet, &state);
    require(muxer != nullptr, "ireader negotiated payload muxer create");

    aom_av1_t av1{};
    av1.marker = 1;
    av1.version = 1;
    av1.seq_profile = 0;
    av1.seq_level_idx_0 = 13;
    av1.chroma_subsampling_x = 1;
    av1.chroma_subsampling_y = 1;
    std::array<std::uint8_t, 4> av1_config{};
    require(aom_av1_codec_configuration_record_save(&av1, av1_config.data(), av1_config.size()) == static_cast<int>(av1_config.size()),
            "ireader negotiated payload av1 config");
    require(rtsp_muxer_add_payload(
                muxer, "RTP/AVP", 90'000, 45, "AV1", 0, 2, 0, av1_config.data(), static_cast<int>(av1_config.size())) >= 0,
            "ireader negotiated payload av1 payload");
    require(rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, 64, "H264", 0, 2, 0, nullptr, 0) >= 0,
            "ireader negotiated payload h264 payload");
    require(rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, 77, "H265", 0, 2, 0, nullptr, 0) >= 0,
            "ireader negotiated payload h265 payload");
    rtsp_muxer_destroy(muxer);

    muxer = rtsp_muxer_create(on_packet, &state);
    require(muxer != nullptr, "ireader negotiated payload override muxer create");
    auto payload = rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, 35, "VP9", 0, 2, 0, nullptr, 0);
    require(payload >= 0, "ireader negotiated payload vp9 payload");
    auto media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_VP9, nullptr, 0);
    require(media >= 0, "ireader negotiated payload vp9 media");
    const std::array<std::uint8_t, 4> vp9_frame{0x01, 0x02, 0x03, 0x04};
    require(rtsp_muxer_input(muxer, media, 0, 0, vp9_frame.data(), static_cast<int>(vp9_frame.size()), 1) == 0,
            "ireader negotiated payload vp9 packetize");
    require(state.packet.size() == 17U && (state.packet[1] & 0x7fU) == 35U && state.packet[12] == 0x0cU,
            "ireader negotiated payload vp9 uses negotiated encoding");
    rtsp_muxer_destroy(muxer);

    state.packet.clear();
    muxer = rtsp_muxer_create(on_packet, &state);
    require(muxer != nullptr, "ireader static payload override muxer create");
    payload = rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, 18, "H264", 0, 2, 0, nullptr, 0);
    require(payload >= 0, "ireader static payload override h264 payload");
    media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_H264, nullptr, 0);
    require(media >= 0, "ireader static payload override h264 media");
    const std::array<std::uint8_t, 8> h264_frame{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
    require(rtsp_muxer_input(muxer, media, 0, 0, h264_frame.data(), static_cast<int>(h264_frame.size()), 1) == 0,
            "ireader static payload override h264 packetize");
    require(state.packet.size() == 16U && (state.packet[1] & 0x7fU) == 18U && state.packet[12] == 0x41U,
            "ireader static payload override uses h264 encoding");
    rtsp_muxer_destroy(muxer);

    muxer = rtsp_muxer_create(on_packet, &state);
    require(muxer != nullptr, "ireader static payload default muxer create");
    require(rtsp_muxer_add_payload(muxer, "RTP/AVP", 8'000, RTP_PAYLOAD_PCMA, "PCMA", 0, 2, 0, nullptr, 0) >= 0,
            "ireader static payload pcma remains supported");
    rtsp_muxer_destroy(muxer);
}

void test_ireader_av1_sdp_payload_type()
{
    constexpr int payload_type = 98;
    aom_av1_t av1{};
    av1.marker = 1;
    av1.version = 1;
    av1.seq_profile = 0;
    av1.seq_level_idx_0 = 13;
    av1.chroma_subsampling_x = 1;
    av1.chroma_subsampling_y = 1;
    std::array<std::uint8_t, 4> config{};
    require(aom_av1_codec_configuration_record_save(&av1, config.data(), config.size()) == static_cast<int>(config.size()),
            "ireader av1 config save");

    rtp_timeline_capture capture;
    auto* muxer = rtsp_muxer_create(&capture_rtp_timestamp, &capture);
    require(muxer != nullptr, "ireader av1 sdp muxer create");
    const auto payload = rtsp_muxer_add_payload(muxer,
                                                 "RTP/AVP",
                                                 90'000,
                                                 payload_type,
                                                 "AV1",
                                                 0,
                                                 0x12345678U,
                                                 0,
                                                 config.data(),
                                                 static_cast<int>(config.size()));
    require(payload >= 0, "ireader av1 sdp add payload");

    std::uint16_t sequence{};
    std::uint32_t timestamp{};
    const char* media_text{};
    int media_text_size{};
    require(rtsp_muxer_getinfo(muxer, payload, &sequence, &timestamp, &media_text, &media_text_size) == 0 && media_text != nullptr &&
                media_text_size > 0,
            "ireader av1 sdp getinfo");
    const std::string_view sdp(media_text, static_cast<std::size_t>(media_text_size));
    require(sdp.contains("m=video 0 RTP/AVP 98\n"), "ireader av1 sdp media payload type");
    require(sdp.contains("a=rtpmap:98 AV1/90000\n"), "ireader av1 sdp rtpmap payload type");
    require(sdp.contains("a=fmtp:98 profile=0;level-idx=13;tier=0"), "ireader av1 sdp fmtp payload type");
    require(rtsp_muxer_destroy(muxer) == 0, "ireader av1 sdp muxer destroy");
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

void test_audio_transcoder_aac_opus()
{
    const audio_transcoder_config config{
        .input = audio_transcoder_format{
            .codec = codec_id::aac,
            .sample_rate = 44'100,
            .channel_count = 2,
        },
        .output = audio_transcoder_format{
            .codec = codec_id::opus,
            .sample_rate = 48'000,
            .channel_count = 2,
        },
        .input_codec_config = aac_asc,
        .output_bit_rate = 128'000,
        .output_cutoff = 20'000,
    };

    audio_transcoder transcoder;
    require(transcoder.startup(config), "audio transcoder startup");

    std::vector<media_frame> output;
    std::int64_t pts_ns = 37'000'000;
    for (const auto& adts : valid_aac_adts_frames)
    {
        require(transcoder.transcode(media_frame{
                                         .track = audio_track_id,
                                         .dts_ns = pts_ns,
                                         .pts_ns = pts_ns,
                                         .key_frame = false,
                                         .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
                                     },
                                     output),
                "audio transcoder continuous input");
        pts_ns += 23'219'954;
    }

    require(!output.empty(), "audio transcoder streaming output");
    const auto streaming_packet_count = output.size();
    require(transcoder.flush(output), "audio transcoder flush");
    require(output.size() > streaming_packet_count, "audio transcoder flush pending output");
    const auto flushed_packet_count = output.size();
    require(transcoder.flush(output), "audio transcoder repeated flush");
    require(output.size() == flushed_packet_count, "audio transcoder repeated flush no duplicate");

    std::vector<media_frame> after_flush;
    require(!transcoder.transcode(media_frame{
                                      .track = audio_track_id,
                                      .dts_ns = pts_ns,
                                      .pts_ns = pts_ns,
                                      .key_frame = false,
                                      .payload = std::make_shared<const std::vector<std::uint8_t>>(valid_aac_adts_frames.front()),
                                  },
                                  after_flush),
            "audio transcoder rejects input after flush");
    require(output.front().track == audio_track_id && output.front().pts_ns == 37'000'000, "audio transcoder output timeline origin");
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        require(output[index].payload && !output[index].payload->empty(), "audio transcoder opus payload");
        require(output[index].dts_ns == output[index].pts_ns, "audio transcoder output dts");
        if (index > 0)
        {
            require(output[index].pts_ns > output[index - 1U].pts_ns, "audio transcoder continuous output timeline");
        }
    }

    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    require(decoder != nullptr, "audio transcoder opus decoder");
    AVCodecContext* decoder_context = avcodec_alloc_context3(decoder);
    require(decoder_context != nullptr, "audio transcoder opus decoder context");
    decoder_context->sample_rate = 48'000;
    av_channel_layout_default(&decoder_context->ch_layout, 2);
    require(avcodec_open2(decoder_context, decoder, nullptr) == 0, "audio transcoder opus decoder open");

    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    require(packet != nullptr && decoded != nullptr, "audio transcoder opus decode buffers");

    std::int64_t decoded_samples = 0;
    const auto receive_decoded = [&]()
    {
        while (true)
        {
            av_frame_unref(decoded);
            const int result = avcodec_receive_frame(decoder_context, decoded);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                return;
            }
            require(result == 0, "audio transcoder opus decode receive");
            require(decoded->sample_rate == 48'000 && decoded->ch_layout.nb_channels == 2, "audio transcoder opus output format");
            require(decoded->nb_samples > 0, "audio transcoder opus decoded samples");
            decoded_samples += decoded->nb_samples;
        }
    };

    for (const auto& encoded : output)
    {
        av_packet_unref(packet);
        require(av_new_packet(packet, static_cast<int>(encoded.payload->size())) == 0, "audio transcoder opus packet allocate");
        std::memcpy(packet->data, encoded.payload->data(), encoded.payload->size());
        require(avcodec_send_packet(decoder_context, packet) == 0, "audio transcoder opus decode send");
        receive_decoded();
    }
    require(avcodec_send_packet(decoder_context, nullptr) == 0, "audio transcoder opus decoder flush");
    receive_decoded();
    const auto minimum_output_samples = av_rescale_rnd(
        static_cast<std::int64_t>(valid_aac_adts_frames.size()) * 1'024, 48'000, 44'100, AV_ROUND_DOWN);
    require(decoded_samples >= minimum_output_samples, "audio transcoder flush preserves tail samples");

    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder_context);

    transcoder.shutdown();
    transcoder.shutdown();
    require(!transcoder.transcode(media_frame{
                                      .track = audio_track_id,
                                      .dts_ns = 0,
                                      .pts_ns = 0,
                                      .key_frame = false,
                                      .payload = std::make_shared<const std::vector<std::uint8_t>>(valid_aac_adts_frames.front()),
                                  },
                                  after_flush),
            "audio transcoder rejects input after shutdown");

    require(transcoder.startup(config), "audio transcoder restart");
    std::vector<media_frame> restarted_output;
    require(!transcoder.transcode(media_frame{
                                      .track = audio_track_id,
                                      .dts_ns = 1'000'000'000,
                                      .pts_ns = 1'000'000'000,
                                      .key_frame = false,
                                      .payload = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{0, 1, 2}),
                                  },
                                  restarted_output),
            "audio transcoder rejects invalid first frame");

    pts_ns = 5'000'000'000;
    for (const auto& adts : valid_aac_adts_frames)
    {
        require(transcoder.transcode(media_frame{
                                         .track = audio_track_id,
                                         .dts_ns = pts_ns,
                                         .pts_ns = pts_ns,
                                         .key_frame = false,
                                         .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
                                     },
                                     restarted_output),
                "audio transcoder input after restart");
        pts_ns += 23'219'954;
    }
    require(transcoder.flush(restarted_output), "audio transcoder flush after restart");
    require(!restarted_output.empty(), "audio transcoder output after restart");
    require(restarted_output.front().pts_ns == 5'000'000'000, "audio transcoder invalid first frame does not start timeline");
    transcoder.shutdown();
    transcoder.shutdown();
}

void validate_av1_transcoder_output(const encoded_video_fixture& source, const std::vector<media_frame>& output)
{
    const AVCodec* decoder = avcodec_find_decoder_by_name("libdav1d");
    if (decoder == nullptr)
    {
        decoder = avcodec_find_decoder(AV_CODEC_ID_AV1);
    }
    require(decoder != nullptr, "video transcoder av1 decoder");
    AVCodecContext* decoder_context = avcodec_alloc_context3(decoder);
    AVCodecContext* parser_context = avcodec_alloc_context3(nullptr);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_AV1);
    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    require(decoder_context != nullptr && parser_context != nullptr && parser != nullptr && packet != nullptr && decoded != nullptr,
            "video transcoder av1 validation allocate");
    decoder_context->pkt_timebase = AVRational{1, 1'000'000'000};
    require(avcodec_open2(decoder_context, decoder, nullptr) == 0, "video transcoder av1 decoder open");

    int decoded_frames{};
    bool first_temporal_unit_sequence_header{};
    bool frame_obu{};
    bool parsed_key{};
    bool color_signaling{};
    std::int64_t dark_luma_sum{};
    std::int64_t bright_luma_sum{};
    std::int64_t luma_sample_count{};
    for (std::size_t frame_index = 0; frame_index < output.size(); ++frame_index)
    {
        const auto& frame = output[frame_index];
        require(frame.track == video_track_id && frame.payload && !frame.payload->empty(), "video transcoder av1 media frame");
        require(frame.pts_ns != AV_NOPTS_VALUE && frame.dts_ns != AV_NOPTS_VALUE, "video transcoder av1 packet timestamps");

        std::size_t offset{};
        while (offset < frame.payload->size())
        {
            const std::uint8_t header = (*frame.payload)[offset++];
            require((header & 0x80U) == 0 && (header & 0x02U) != 0, "video transcoder av1 low-overhead obu header");
            const auto obu_type = static_cast<std::uint8_t>((header >> 3U) & 0x0fU);
            if ((header & 0x04U) != 0)
            {
                require(offset < frame.payload->size(), "video transcoder av1 obu extension");
                ++offset;
            }
            std::uint64_t obu_size{};
            unsigned shift{};
            while (true)
            {
                require(offset < frame.payload->size() && shift <= 56U, "video transcoder av1 obu leb128");
                const std::uint8_t value = (*frame.payload)[offset++];
                obu_size |= static_cast<std::uint64_t>(value & 0x7fU) << shift;
                if ((value & 0x80U) == 0)
                {
                    break;
                }
                shift += 7U;
            }
            require(obu_size <= frame.payload->size() - offset, "video transcoder av1 obu boundary");
            first_temporal_unit_sequence_header = first_temporal_unit_sequence_header || (frame_index == 0 && obu_type == 1U);
            frame_obu = frame_obu || obu_type == 3U || obu_type == 6U;
            offset += static_cast<std::size_t>(obu_size);
        }
        require(offset == frame.payload->size(), "video transcoder av1 complete temporal unit");

        std::uint8_t* parsed_data{};
        int parsed_size{};
        const int consumed = av_parser_parse2(parser,
                                              parser_context,
                                              &parsed_data,
                                              &parsed_size,
                                              frame.payload->data(),
                                              static_cast<int>(frame.payload->size()),
                                              frame.pts_ns,
                                              frame.dts_ns,
                                              0);
        require(consumed == static_cast<int>(frame.payload->size()) && parsed_size == static_cast<int>(frame.payload->size()),
                "video transcoder av1 parser temporal unit");
        require(parser->width == source.width && parser->height == source.height, "video transcoder av1 parser dimensions");
        if (frame.key_frame)
        {
            parsed_key = parser->key_frame == 1;
        }

        av_packet_unref(packet);
        require(av_new_packet(packet, static_cast<int>(frame.payload->size())) == 0, "video transcoder av1 decode packet allocate");
        std::memcpy(packet->data, frame.payload->data(), frame.payload->size());
        packet->pts = frame.pts_ns;
        packet->dts = frame.dts_ns;
        require(avcodec_send_packet(decoder_context, packet) == 0, "video transcoder av1 decode send");
        while (true)
        {
            av_frame_unref(decoded);
            const int result = avcodec_receive_frame(decoder_context, decoded);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                break;
            }
            require(result == 0 && decoded->width == source.width && decoded->height == source.height,
                    "video transcoder av1 decoded dimensions");
            if (source.codec == codec_id::h264)
            {
                color_signaling = decoded->color_range == AVCOL_RANGE_JPEG && decoded->color_primaries == AVCOL_PRI_BT709 &&
                    decoded->color_trc == AVCOL_TRC_BT709 && decoded->colorspace == AVCOL_SPC_BT709;
                for (int y = 0; y < decoded->height; ++y)
                {
                    const auto* row = decoded->data[0] + y * decoded->linesize[0];
                    if (y < decoded->height / 4 || y >= decoded->height * 3 / 4)
                    {
                        const auto row_sum = std::accumulate(row, row + decoded->width, std::int64_t{});
                        if (y < decoded->height / 4)
                        {
                            dark_luma_sum += row_sum;
                        }
                        else
                        {
                            bright_luma_sum += row_sum;
                        }
                        luma_sample_count += decoded->width;
                    }
                }
            }
            ++decoded_frames;
        }
    }

    require(avcodec_send_packet(decoder_context, nullptr) == 0, "video transcoder av1 decoder flush");
    while (true)
    {
        av_frame_unref(decoded);
        const int result = avcodec_receive_frame(decoder_context, decoded);
        if (result == AVERROR_EOF)
        {
            break;
        }
        require(result == 0 && decoded->width == source.width && decoded->height == source.height,
                "video transcoder av1 flush dimensions");
        if (source.codec == codec_id::h264)
        {
            color_signaling = decoded->color_range == AVCOL_RANGE_JPEG && decoded->color_primaries == AVCOL_PRI_BT709 &&
                decoded->color_trc == AVCOL_TRC_BT709 && decoded->colorspace == AVCOL_SPC_BT709;
            for (int y = 0; y < decoded->height; ++y)
            {
                const auto* row = decoded->data[0] + y * decoded->linesize[0];
                if (y < decoded->height / 4 || y >= decoded->height * 3 / 4)
                {
                    const auto row_sum = std::accumulate(row, row + decoded->width, std::int64_t{});
                    if (y < decoded->height / 4)
                    {
                        dark_luma_sum += row_sum;
                    }
                    else
                    {
                        bright_luma_sum += row_sum;
                    }
                    luma_sample_count += decoded->width;
                }
            }
        }
        ++decoded_frames;
    }

    require(first_temporal_unit_sequence_header && frame_obu, "video transcoder av1 first temporal unit sequence and frame obu");
    require(source.codec != codec_id::h264 || color_signaling, "video transcoder av1 color signaling");
    require(source.codec != codec_id::h264 ||
                (luma_sample_count > 0 && dark_luma_sum / (luma_sample_count / 2) <= 8 &&
                 bright_luma_sum / (luma_sample_count / 2) >= 247),
            "video transcoder av1 full range pixels");
    require(parsed_key, "video transcoder av1 packet key flag matches parser");
    require(decoded_frames == static_cast<int>(source.frames.size()), "video transcoder av1 decoded frame count");
    av_frame_free(&decoded);
    av_packet_free(&packet);
    av_parser_close(parser);
    avcodec_free_context(&parser_context);
    avcodec_free_context(&decoder_context);
}

void test_video_transcoder_h26x_av1()
{
    for (const auto input_codec : {codec_id::h264, codec_id::h265})
    {
        const auto source = make_video_transcoder_fixture(input_codec);
        const video_transcoder_config config{
            .input_codec = input_codec,
            .output_codec = codec_id::av1,
            .input_codec_config = source.codec_config,
        };

        video_transcoder transcoder;
        require(transcoder.startup(config), "video transcoder startup");
        std::vector<media_frame> output;
        for (const auto& frame : source.frames)
        {
            require(transcoder.transcode(frame, output), "video transcoder input frame");
        }
        const auto before_flush = output.size();
        require(transcoder.flush(output), "video transcoder flush");
        require(output.size() == source.frames.size() && output.size() > before_flush, "video transcoder delayed flush output");
        const auto after_flush = output.size();
        require(transcoder.flush(output) && output.size() == after_flush, "video transcoder repeated flush");
        require(!transcoder.transcode(source.frames.front(), output), "video transcoder rejects input after flush");
        require(std::ranges::any_of(output, [](const media_frame& frame) { return frame.key_frame; }), "video transcoder key packet");
        require(output.front().pts_ns >= 5'000'000'000 && output.front().dts_ns >= 5'000'000'000,
                "video transcoder nonzero timeline");
        for (std::size_t index = 1; index < output.size(); ++index)
        {
            require(output[index].pts_ns > output[index - 1].pts_ns, "video transcoder monotonic pts");
        }
        validate_av1_transcoder_output(source, output);

        video_transcoder invalid_timeline;
        require(invalid_timeline.startup(config), "video transcoder invalid timeline startup");
        auto invalid = source.frames.front();
        invalid.pts_ns = AV_NOPTS_VALUE;
        require(!invalid_timeline.transcode(invalid, output), "video transcoder invalid first timeline");
        std::vector<media_frame> recovered;
        for (const auto& frame : source.frames)
        {
            require(invalid_timeline.transcode(frame, recovered), "video transcoder timeline recovery");
        }
        require(invalid_timeline.flush(recovered) && recovered.size() == source.frames.size(), "video transcoder timeline recovery flush");

        video_transcoder malformed;
        require(malformed.startup(config), "video transcoder malformed startup");
        auto bad = source.frames.front();
        bad.payload = std::make_shared<const std::vector<std::uint8_t>>(16U, 0xffU);
        std::vector<media_frame> rejected;
        require(!malformed.transcode(bad, rejected) && malformed.flush(rejected) && rejected.empty(),
                "video transcoder malformed annex-b rejected");

        video_transcoder restarted;
        require(restarted.startup(config), "video transcoder new generation startup");
        std::vector<media_frame> restarted_output;
        for (const auto& frame : source.frames)
        {
            require(restarted.transcode(frame, restarted_output), "video transcoder new generation input");
        }
        require(restarted.flush(restarted_output) && restarted_output.size() == source.frames.size(),
                "video transcoder new generation output");
        restarted.shutdown();
        restarted.shutdown();

        video_transcoder long_running;
        require(long_running.startup(config), "video transcoder long timeline startup");
        std::vector<media_frame> long_running_output;
        constexpr std::int64_t cycle_duration_ns = 200'000'000;
        constexpr int cycle_count = 30;
        for (int cycle = 0; cycle < cycle_count; ++cycle)
        {
            for (const auto& frame : source.frames)
            {
                auto shifted = frame;
                shifted.pts_ns += static_cast<std::int64_t>(cycle) * cycle_duration_ns;
                shifted.dts_ns += static_cast<std::int64_t>(cycle) * cycle_duration_ns;
                require(long_running.transcode(shifted, long_running_output), "video transcoder long timeline frame");
            }
        }
        require(long_running.flush(long_running_output), "video transcoder long timeline flush");
        require(long_running_output.size() == source.frames.size() * cycle_count, "video transcoder long timeline output");
        require(long_running_output.back().pts_ns - long_running_output.front().pts_ns > 4'294'967'295LL,
                "video transcoder long timeline exceeds libaom nanosecond boundary");
    }

    const auto source = make_video_transcoder_fixture(codec_id::h264);
    video_transcoder constrained;
    require(constrained.startup(video_transcoder_config{
                .input_codec = codec_id::h264,
                .output_codec = codec_id::av1,
                .input_codec_config = source.codec_config,
                .av1 = av1_encoding_parameters{
                    .profile = 0,
                    .level_idx = 13,
                    .tier = 0,
                },
            }),
            "video transcoder av1 parameters startup");
    std::vector<media_frame> constrained_output;
    for (const auto& frame : source.frames)
    {
        require(constrained.transcode(frame, constrained_output), "video transcoder av1 parameters frame");
    }
    require(constrained.flush(constrained_output) && !constrained_output.empty(), "video transcoder av1 parameters flush");
    aom_av1_t av1{};
    require(aom_av1_codec_configuration_record_init(
                &av1, constrained_output.front().payload->data(), constrained_output.front().payload->size()) == 0,
            "video transcoder av1 parameters sequence header");
    require(av1.seq_profile == 0 && av1.seq_level_idx_0 <= 13 && av1.seq_tier_0 == 0,
            "video transcoder av1 parameters stream properties");

    video_transcoder unsupported_profile;
    require(!unsupported_profile.startup(video_transcoder_config{
                .input_codec = codec_id::h264,
                .output_codec = codec_id::av1,
                .input_codec_config = source.codec_config,
                .av1 = av1_encoding_parameters{
                    .profile = 1,
                    .level_idx = 13,
                    .tier = 0,
                },
            }),
            "video transcoder av1 unsupported profile");

    video_transcoder unsupported;
    require(!unsupported.startup(video_transcoder_config{
                .input_codec = codec_id::av1,
                .output_codec = codec_id::h264,
                .input_codec_config = {0, 0, 0, 1},
            }),
            "video transcoder unsupported pair");
}

void test_media_stream_configless_audio_track()
{
    boost::asio::io_context io;
    boost::asio::post(io,
                      [&]()
                      {
                          auto stream = std::make_shared<media_stream>("live/configless-audio", io.get_executor());
                          media_track track{
                              .id = audio_track_id,
                              .kind = media_kind::audio,
                              .codec = codec_id::opus,
                              .clock_rate = 48'000,
                              .channel_count = 1,
                              .codec_config = {},
                              .config_version = 0,
                          };
                          require(stream->set_tracks({track}), "configless audio track accepted");
                          require(stream->tracks().front().config_version == 1, "configless audio initial config version");

                          track.codec = codec_id::g711a;
                          require(!stream->update_track(track), "configless audio codec change rejected");

                          track.codec = codec_id::opus;
                          track.channel_count = 2;
                          require(stream->update_track(track), "configless audio track update accepted");
                          const auto tracks = stream->tracks();
                          require(tracks.front().channel_count == 2 && tracks.front().config_version == 2, "configless audio update published");
                      });
    io.run();
}

void test_media_stream_sink_lifecycle()
{
    boost::asio::io_context io;
    boost::asio::post(io,
                      [&]()
                      {
                          auto stream = std::make_shared<media_stream>("live/test", io.get_executor());
                          auto first_track = make_video_track();
                          first_track.config_version = 99;
                          require(stream->set_tracks({std::move(first_track)}), "sink lifecycle video track");
                          require(!stream->set_tracks({make_video_track(), make_audio_track()}), "initial topology can only be published once");
                          require(stream->tracks().front().config_version == 1, "stream owns initial config version");

                          auto sink = std::make_shared<counting_sink>();
                          stream->add_sink(sink);
                          require(sink->tracks == 1, "sink receives current track on attach");

                          auto duplicate = std::make_shared<counting_sink>();
                          stream->add_sink(duplicate);
                          require(duplicate->tracks == 0, "stream keeps one source-owner sink");

                          auto owned_stream = std::make_shared<media_stream>("live/owned-sink", io.get_executor());
                          require(owned_stream->set_tracks({make_video_track()}), "owned sink track");
                          std::weak_ptr<media_sink> owned_sink;
                          {
                              auto current = std::make_shared<counting_sink>();
                              owned_sink = current;
                              owned_stream->add_sink(current);
                          }
                          require(!owned_sink.expired(), "stream strongly owns source-owner sink");
                          owned_stream->end();
                          require(owned_sink.expired(), "stream releases source-owner sink after end");

                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_video_frame(40'000'000, false));
                          require(sink->frames == 2, "sink receives live frames");

                          require(!stream->update_track(make_audio_track()), "runtime track addition rejected");
                          require(!stream->update_track(make_video_track()), "identical config ignored");
                          auto changed_kind = make_video_track(2);
                          changed_kind.kind = media_kind::audio;
                          require(!stream->update_track(std::move(changed_kind)), "track kind change rejected");
                          auto changed_codec = make_video_track(2);
                          changed_codec.codec = codec_id::aac;
                          require(!stream->update_track(std::move(changed_codec)), "track codec change rejected");
                          auto second_track = make_video_track(2);
                          second_track.config_version = 99;
                          require(stream->update_track(std::move(second_track)), "sink lifecycle config update");
                          require(stream->tracks().front().config_version == 2, "stream increments config version");
                          require(!stream->update_track(make_video_track(2)), "changed config duplicate ignored");
                          require(sink->tracks == 2, "sink receives only actual config updates");

                          stream->end();
                          stream->end();
                          require(sink->ends == 1, "sink receives one stream end");

                          stream->publish(make_video_frame(80'000'000, false));
                          require(sink->frames == 2, "ended stream stops sink media");

                          auto late = std::make_shared<counting_sink>();
                          stream->add_sink(late);
                          require(late->tracks == 0 && late->frames == 0 && late->ends == 1, "sink attached after end receives terminal callback");
                      });
    io.run();
}

void test_media_stream_sink_gop_replay()
{
    boost::asio::io_context io;
    boost::asio::post(io,
                      [&]()
                      {
                          auto stream = std::make_shared<media_stream>("live/gop", io.get_executor());
                          require(stream->set_tracks({make_video_track(), make_audio_track()}), "gop tracks");
                          stream->publish(make_audio_frame(-20'000'000));
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_audio_frame(20'000'000));
                          stream->publish(make_video_frame(40'000'000, false));

                          auto sink = std::make_shared<counting_sink>();
                          stream->add_sink(sink);
                          require(sink->tracks == 2, "gop replays track configuration first");
                          require(sink->received_frames ==
                                      std::vector<std::pair<track_id, std::int64_t>>{
                                          {video_track_id, 0},
                                          {audio_track_id, 20'000'000},
                                          {video_track_id, 40'000'000},
                                  },
                                  "gop replays frames from current key frame");

                          auto latest_stream = std::make_shared<media_stream>("live/gop-latest", io.get_executor());
                          require(latest_stream->set_tracks({make_video_track(), make_audio_track()}), "latest gop tracks");
                          latest_stream->publish(make_video_frame(0, true));
                          latest_stream->publish(make_video_frame(40'000'000, false));
                          latest_stream->publish(make_video_frame(1'000'000'000, true));
                          latest_stream->publish(make_audio_frame(1'020'000'000));
                          latest_stream->publish(make_video_frame(1'040'000'000, false));
                          auto latest_sink = std::make_shared<counting_sink>();
                          latest_stream->add_sink(latest_sink);
                          require(latest_sink->received_frames ==
                                      std::vector<std::pair<track_id, std::int64_t>>{
                                          {video_track_id, 1'000'000'000},
                                          {audio_track_id, 1'020'000'000},
                                          {video_track_id, 1'040'000'000},
                                      },
                                  "late sink receives only current gop");

                          auto blocked_stream = std::make_shared<media_stream>("live/gop-blocked", io.get_executor());
                          require(blocked_stream->set_tracks({make_video_track(), make_audio_track()}), "blocked gop tracks");
                          blocked_stream->publish(make_video_frame(0, true));
                          blocked_stream->publish(make_audio_frame(20'000'000));
                          blocked_stream->publish(make_video_frame(40'000'000, false));
                          require(blocked_stream->update_track(make_audio_track(2)), "blocked gop audio config update");
                          auto blocked_sink = std::make_shared<counting_sink>();
                          blocked_stream->add_sink(blocked_sink);
                          require(blocked_sink->frames == 0, "sink replay blocked after track config change");

                          auto resumed_stream = std::make_shared<media_stream>("live/gop-resumed", io.get_executor());
                          require(resumed_stream->set_tracks({make_video_track(), make_audio_track()}), "resumed gop tracks");
                          resumed_stream->publish(make_video_frame(0, true));
                          resumed_stream->publish(make_video_frame(40'000'000, false));
                          require(resumed_stream->update_track(make_audio_track(2)), "resumed gop audio config update");
                          resumed_stream->publish(make_video_frame(80'000'000, false));
                          resumed_stream->publish(make_video_frame(2'000'000'000, true));
                          resumed_stream->publish(make_audio_frame(2'020'000'000));
                          resumed_stream->publish(make_video_frame(2'040'000'000, false));
                          auto resumed_sink = std::make_shared<counting_sink>();
                          resumed_stream->add_sink(resumed_sink);
                          require(resumed_sink->received_frames ==
                                      std::vector<std::pair<track_id, std::int64_t>>{
                                          {video_track_id, 2'000'000'000},
                                          {audio_track_id, 2'020'000'000},
                                          {video_track_id, 2'040'000'000},
                                      },
                                  "sink replay resumes from next key frame after track config change");

                          auto overflow_stream = std::make_shared<media_stream>("live/gop-overflow", io.get_executor());
                          require(overflow_stream->set_tracks({make_video_track()}), "gop overflow track");
                          overflow_stream->publish(make_video_frame(0, true));
                          for (std::int64_t index = 1; index <= 2500; ++index)
                          {
                              overflow_stream->publish(make_video_frame(index, false));
                          }
                          auto overflow_sink = std::make_shared<counting_sink>();
                          overflow_stream->add_sink(overflow_sink);
                          require(overflow_sink->frames == 0, "gop overflow drops incomplete cache");

                          auto h265_stream = std::make_shared<media_stream>("live/gop-h265", io.get_executor());
                          require(h265_stream->set_tracks({make_h265_track()}), "gop h265 track");
                          h265_stream->publish(make_h265_frame(0, true));
                          h265_stream->publish(make_h265_frame(40'000'000, false));
                          auto h265_sink = std::make_shared<counting_sink>();
                          h265_stream->add_sink(h265_sink);
                          require(h265_sink->received_frames ==
                                      std::vector<std::pair<track_id, std::int64_t>>{{video_track_id, 0}, {video_track_id, 40'000'000}},
                                  "gop h265 replay");
                      });
    io.run();
}

void test_media_stream_sink_owner_affinity()
{
    io_context_pool workers(2);
    auto stream = std::make_shared<media_stream>("live/threaded", workers.context(0).get_executor());

    std::atomic_int ended_count{};
    auto sink = std::make_shared<worker_sink>(ended_count, workers);
    const void* first_payload{};
    const void* second_payload{};

    boost::asio::post(workers.context(0),
                      [&, stream]()
                      {
                          require(stream->set_tracks({make_video_track()}), "threaded stream track");
                          stream->add_sink(sink);

                          auto first_frame = make_video_frame(0, true);
                          first_payload = first_frame.payload.get();
                          stream->publish(std::move(first_frame));
                          auto second_frame = make_video_frame(40'000'000, false);
                          second_payload = second_frame.payload.get();
                          stream->publish(std::move(second_frame));
                          stream->end();

                          require(first_payload != nullptr && second_payload != nullptr, "threaded payload identity source");
                      });
    workers.run();

    require(sink->tracks() == 1, "threaded track replay");
    require(sink->frames() == std::vector<std::int64_t>{0, 40'000'000}, "threaded frame order");
    require(sink->ends() == 1, "threaded stream end");
    require(sink->thread() != std::thread::id{}, "sink callback uses stream owner thread");
    const auto payloads = sink->payloads();
    require(payloads == std::vector<const void*>{first_payload, second_payload}, "threaded sink shares frame payload");
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
                          require(stream->set_tracks({make_video_track()}), "pull overrun track");
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
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0},
                    {1, 40'000'000},
                    {1, 1'000'000'000},
                    {1, 1'040'000'000},
                    {1, 2'000'000'000},
                },
            "fast pull reader receives every frame");
    require(stalled->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {1, 2'000'000'000}},
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
                          require(stream->set_tracks({make_video_track()}), "pull duplicate track");
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
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

    reader->request();
    reader->request();
    reader->request();
    drain(owner);

    reader->request();
    reader->request();
    reader->request();
    drain(owner);
    require(reader->frames().empty(), "posted pull callback waits for reader executor");
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {1, 40'000'000}, {1, 80'000'000}},
            "duplicate pull requests produce one bounded batch");
    require(reader->batch_sizes() == std::vector<std::size_t>{3}, "duplicate pull requests keep one outstanding batch");

    reader->request();
    drain(owner);
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {1, 40'000'000}, {1, 80'000'000}},
            "next pull waits at live edge after batch completion");
}

void test_media_stream_pull_reader_batch_limit_and_worker_filtering()
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

    auto stream = std::make_shared<media_stream>("live/pull-batch", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(false, false, std::vector<track_id>{video_track_id});
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          require(stream->set_tracks({make_video_track(), make_audio_track()}), "pull batch tracks");
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                          for (std::size_t index = 0; index < 300; ++index)
                          {
                              const auto pts = static_cast<std::int64_t>(index) * 20'000'000;
                              if ((index % 2U) == 0U)
                              {
                                  stream->publish(make_video_frame(pts, index == 0));
                              }
                              else
                              {
                                  stream->publish(make_audio_frame(pts));
                              }
                          }
                      });
    drain(owner);
    drain(reader_worker);

    reader->request();
    drain(owner);
    drain(reader_worker);
    require(reader->batch_sizes() == std::vector<std::size_t>{128}, "first pull batch is capped at 128 history entries");
    require(reader->frames().size() == 64U, "video reader filters audio after first batch reaches reader worker");

    reader->request();
    drain(owner);
    drain(reader_worker);
    require(reader->batch_sizes() == std::vector<std::size_t>{128, 128}, "second pull batch is capped at 128 history entries");
    require(reader->frames().size() == 128U, "video reader keeps cursor across filtered second batch");

    reader->request();
    drain(owner);
    drain(reader_worker);
    require(reader->batch_sizes() == std::vector<std::size_t>{128, 128, 44}, "final pull batch returns remaining history immediately");
    require(reader->frames().size() == 150U, "worker filtering preserves every subscribed video frame");
}

void test_media_stream_pull_reader_initial_cursor_starts_current_gop()
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

    auto stream = std::make_shared<media_stream>("live/pull-initial-cursor", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(false);
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          require(stream->set_tracks({make_video_track()}), "pull initial cursor track");
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_video_frame(40'000'000, false));
                          stream->publish(make_video_frame(1'000'000'000, true));
                          stream->publish(make_video_frame(1'040'000'000, false));
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });
    drain(owner);
    drain(reader_worker);
    drain(owner);
    drain(reader_worker);

    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 1'000'000'000}, {1, 1'040'000'000}},
            "initial reader cursor starts at current gop instead of retained previous gop");
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
                          require(stream->set_tracks({make_video_track()}), "pull continuity track");
                          static_cast<void>(stream->add_reader(continuity_reader, reader_worker.get_executor()));
                          static_cast<void>(stream->add_reader(overrun_reader, reader_worker.get_executor()));
                          stream->publish(make_video_frame(0, true));
                      });
    drain(owner);
    drain(reader_worker);

    continuity_reader->request();
    overrun_reader->request();
    drain(owner);
    drain(reader_worker);

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(40'000'000, false));
                          stream->publish(make_video_frame(80'000'000, false));
                      });
    drain(owner);
    continuity_reader->request();
    drain(owner);
    drain(reader_worker);

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(1'000'000'000, true));
                          stream->publish(make_video_frame(1'040'000'000, false));
                      });
    drain(owner);
    continuity_reader->request();
    drain(owner);
    drain(reader_worker);
    require(continuity_reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0},
                    {1, 40'000'000},
                    {1, 80'000'000},
                    {1, 1'000'000'000},
                    {1, 1'040'000'000},
                },
            "reader batch cursor continues through previous gop");

    boost::asio::post(owner, [stream]() { stream->publish(make_video_frame(2'000'000'000, true)); });
    drain(owner);
    overrun_reader->request();
    drain(owner);
    drain(reader_worker);
    require(overrun_reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {1, 2'000'000'000}},
            "overrun reader resumes at current gop key frame");
}

void test_media_stream_add_reader_after_end()
{
    boost::asio::io_context owner(1);
    boost::asio::io_context consumer_worker(1);
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
    boost::asio::post(owner, [stream]() { stream->end(); });
    boost::asio::post(consumer_worker,
                      [stream, reader, &reader_worker]()
                      {
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });

    drain(consumer_worker);
    drain(owner);
    drain(reader_worker);

    require(reader->track_versions().empty(), "ended stream reader receives no tracks");
    require(reader->ready_generations().empty(), "ended stream reader is never ready");
    require(reader->frames().empty(), "ended stream reader receives no frames");
    require(reader->ends() == 1, "queued late reader receives one terminal event");
}

void test_media_stream_pull_reader_track_snapshot_order()
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
                          require(stream->set_tracks({make_video_track()}), "pull generation initial track");
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

    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}},
            "pre-update batch remains valid before reader-local config change");
    require(reader->track_versions() ==
                std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}, {2, 2}},
            "track update advances reader-local generation in callback order");
    require(reader->ready_generations() == std::vector<std::uint64_t>{1, 2}, "track update publishes reader-local ready generation");

    drain(owner);
    boost::asio::post(owner, [stream]() { stream->publish(make_video_frame(1'000'000'000, true)); });
    drain(owner);
    drain(reader_worker);
    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {2, 1'000'000'000}},
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
                std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 0}, {2, 1'000'000'000}},
            "end suppresses old posted batch");
    require(reader->ends() == 1 && reader->end_generation() == 3, "end is one terminal reader event");

    auto remove_stream = std::make_shared<media_stream>("live/pull-remove", owner.get_executor());
    auto removed_reader = std::make_shared<pull_test_reader>(true);
    boost::asio::post(owner,
                      [remove_stream, removed_reader, &reader_worker]()
                      {
                          require(remove_stream->set_tracks({make_video_track()}), "pull remove track");
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

void test_media_stream_reader_track_interest()
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
    const auto drain_all = [&]()
    {
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            drain(owner);
            drain(reader_worker);
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-interest", owner.get_executor());
    auto video_reader = std::make_shared<pull_test_reader>(true, true, std::vector<track_id>{video_track_id});
    auto full_reader = std::make_shared<pull_test_reader>(true);
    auto late_reader = std::make_shared<pull_test_reader>(true);
    boost::asio::post(owner,
                      [stream, video_reader, full_reader, &reader_worker]()
                      {
                          require(stream->set_tracks({make_video_track(), make_audio_track()}), "pull interest tracks");
                          static_cast<void>(stream->add_reader(video_reader, reader_worker.get_executor()));
                          static_cast<void>(stream->add_reader(full_reader, reader_worker.get_executor()));
                      });
    drain_all();

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_audio_frame(20'000'000));
                          stream->publish(make_video_frame(40'000'000, false));
                      });
    drain_all();

    boost::asio::post(owner,
                      [stream, late_reader, &reader_worker]()
                      {
                          require(stream->update_track(make_audio_track(2)), "pull interest audio config update");
                          stream->publish(make_video_frame(80'000'000, false));
                          stream->publish(make_audio_frame(100'000'000));
                          stream->publish(make_video_frame(120'000'000, false));
                          static_cast<void>(stream->add_reader(late_reader, reader_worker.get_executor()));
                      });
    drain_all();

    require(video_reader->ready_generations() == std::vector<std::uint64_t>{1},
            "irrelevant track update keeps subset reader local generation");
    require(video_reader->track_versions() ==
                std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}},
            "subset reader receives only interested track config");
    require(video_reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0}, {1, 40'000'000}, {1, 80'000'000}, {1, 120'000'000}},
            "irrelevant track update preserves subset reader cursor");

    require(full_reader->ready_generations() == std::vector<std::uint64_t>{1, 2},
            "full reader advances local generation for audio config update");
    require(full_reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0}, {1, 20'000'000}, {1, 40'000'000}, {2, 80'000'000}, {2, 100'000'000}, {2, 120'000'000}},
            "full reader filters stale audio and continues at current config");
    require(late_reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0}, {1, 40'000'000}, {1, 80'000'000}, {1, 100'000'000}, {1, 120'000'000}},
            "new full reader skips history from obsolete track config");
}

void test_media_stream_video_keyframe_barrier_is_sticky()
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
    const auto drain_all = [&]()
    {
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            drain(owner);
            drain(reader_worker);
        }
    };

    auto stream = std::make_shared<media_stream>("live/pull-sticky-keyframe", owner.get_executor());
    auto reader = std::make_shared<pull_test_reader>(true);
    boost::asio::post(owner,
                      [stream, reader, &reader_worker]()
                      {
                          require(stream->set_tracks({make_video_track(), make_audio_track()}), "sticky barrier initial tracks");
                          static_cast<void>(stream->add_reader(reader, reader_worker.get_executor()));
                      });
    drain_all();

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(0, true));
                          stream->publish(make_audio_frame(20'000'000));
                          stream->publish(make_video_frame(40'000'000, false));
                      });
    drain_all();
    const std::vector<std::pair<std::uint64_t, std::int64_t>> initial_frames{
        {1, 0}, {1, 20'000'000}, {1, 40'000'000}};
    require(reader->frames() == initial_frames, "sticky barrier initial media");

    boost::asio::post(owner,
                      [stream]()
                      {
                          require(stream->update_track(make_video_track(2)), "sticky barrier video config update");
                          require(stream->update_track(make_video_track(3)), "sticky barrier repeated video config update");
                          require(stream->update_track(make_audio_track(2)), "sticky barrier audio config update");
                          stream->publish(make_audio_frame(60'000'000));
                          stream->publish(make_video_frame(80'000'000, false));
                          stream->publish(make_video_frame(120'000'000, false));
                      });
    drain_all();
    require(reader->frames() == initial_frames, "audio reset cannot release pending video keyframe barrier");

    boost::asio::post(owner,
                      [stream]()
                      {
                          stream->publish(make_video_frame(1'000'000'000, true));
                          stream->publish(make_audio_frame(1'020'000'000));
                          stream->publish(make_video_frame(1'040'000'000, false));
                      });
    drain_all();
    require(reader->frames() ==
                std::vector<std::pair<std::uint64_t, std::int64_t>>{
                    {1, 0}, {1, 20'000'000}, {1, 40'000'000}, {4, 1'000'000'000}, {4, 1'020'000'000}, {4, 1'040'000'000}},
            "current video keyframe releases sticky barrier");
}

void test_stream_registry_generation_lifecycle()
{
    boost::asio::io_context io;
    stream_registry registry;

    auto first = std::make_shared<media_stream>("live/generation", io.get_executor());
    auto second = std::make_shared<media_stream>("live/generation", io.get_executor());
    require(!registry.add(first), "registry rejects stream without tracks");
    require(first->set_tracks({make_video_track()}), "registry first generation track");
    require(second->set_tracks({make_video_track()}), "registry second generation track");
    require(registry.add(first), "registry first generation add");
    require(!registry.add(second), "registry active generation duplicate reject");
    require(registry.find("live/generation").get() == first.get(), "registry first generation remains");

    registry.remove(*first);
    require(!registry.find("live/generation"), "registry remove hides old generation");
    require(registry.add(second), "registry replacement generation add");
    require(registry.find("live/generation").get() == second.get(), "registry replacement generation visible");

    boost::asio::post(io, [first]() { first->end(); });
    io.run();
    require(registry.find("live/generation").get() == second.get(), "old generation end preserves replacement");
    registry.remove(*first);
    require(registry.find("live/generation").get() == second.get(), "registry stale remove preserves replacement");

    registry.remove(*second);
    require(!registry.find("live/generation"), "registry replacement removed");
}

void test_hls_output()
{
    hls_output output(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
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
    const auto ended_at = output.ended_at();
    require(ended_at.has_value(), "hls end timestamp");
    const auto ended_segment_count = output.segment_count();
    output.on_end();
    output.on_frame(make_video_frame(3'000'000'000, true));
    require(output.segment_count() == ended_segment_count && output.ended_at() == ended_at, "hls end is terminal and idempotent");

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

    hls_output reconfigured(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
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

    hls_output signed_timeline(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
    signed_timeline.on_track(make_video_track());
    signed_timeline.on_frame(make_video_frame(-500'000'000, true));
    signed_timeline.on_frame(make_video_frame(500'000'000, true));
    require(signed_timeline.segment_count() == 1U, "hls signed pts reaches target duration");
    signed_timeline.on_end();

    hls_output reordered_video(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
    reordered_video.on_track(make_video_track());
    reordered_video.on_frame(make_video_frame(0, true));
    auto future = make_video_frame(80'000'000, false);
    future.dts_ns = 40'000'000;
    reordered_video.on_frame(std::move(future));
    auto reordered = make_video_frame(40'000'000, false);
    reordered.dts_ns = 80'000'000;
    reordered_video.on_frame(std::move(reordered));
    reordered_video.on_end();
    const auto reordered_playlist = reordered_video.playlist("/hls/reordered");
    require(reordered_playlist.find("#EXTINF:0.080,") != std::string::npos, "hls final segment duration uses maximum presentation timestamp");

}

void test_hls_av1_fmp4_output()
{
    for (const auto input_codec : {codec_id::h264, codec_id::h265})
    {
        const auto source = make_video_transcoder_fixture(input_codec);
        hls_output output(hls_config{
            .target_duration_seconds = 1.0,
            .window_size = 4,
            .video = output_video_config{
                .codec = output_video_codec::av1,
            },
        });
        output.on_track(media_track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = input_codec,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = source.codec_config,
        });
        auto malformed = source.frames.front();
        malformed.payload = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{0x01, 0x02, 0x03, 0x04});
        output.on_frame(malformed);
        if (input_codec == codec_id::h264)
        {
            output.on_track(make_audio_track());
        }
        for (const auto& frame : source.frames)
        {
            output.on_frame(frame);
        }
        if (input_codec == codec_id::h264)
        {
            output.on_frame(make_audio_frame(source.frames.back().pts_ns + 20'000'000));
        }
        output.on_end();

        const auto init = output.init_segment();
        const auto segment = output.segment(0);
        require(init.has_value() && !init->empty(), "hls av1 init segment");
        require(segment.has_value() && !segment->empty(), "hls av1 media segment");
        const auto playlist = output.playlist("/hls/av1");
        require(playlist.find("#EXT-X-VERSION:7") != std::string::npos, "hls av1 playlist version");
        require(playlist.find("#EXT-X-MAP:URI=\"/hls/av1/init.mp4?v=0\"") != std::string::npos, "hls av1 init map");
        require(playlist.find("/hls/av1/0.m4s") != std::string::npos, "hls av1 media uri");

        struct memory_reader
        {
            std::vector<std::uint8_t> data;
            std::size_t position{};
        } memory{.data = *init};
        memory.data.insert(memory.data.end(), segment->begin(), segment->end());
        const mov_buffer_t buffer{
            .read = [](void* param, void* data, std::uint64_t bytes) -> int {
                auto& reader = *static_cast<memory_reader*>(param);
                if (bytes > reader.data.size() - std::min(reader.position, reader.data.size()))
                {
                    return -1;
                }
                std::memcpy(data, reader.data.data() + reader.position, static_cast<std::size_t>(bytes));
                reader.position += static_cast<std::size_t>(bytes);
                return 0;
            },
            .write = nullptr,
            .seek = [](void* param, std::int64_t offset) -> int {
                auto& reader = *static_cast<memory_reader*>(param);
                const auto size = static_cast<std::int64_t>(reader.data.size());
                const auto position = offset >= 0 ? offset : size + offset;
                if (position < 0 || static_cast<std::uint64_t>(position) > reader.data.size())
                {
                    return -1;
                }
                reader.position = static_cast<std::size_t>(position);
                return 0;
            },
            .tell = [](void* param) -> std::int64_t {
                return static_cast<std::int64_t>(static_cast<memory_reader*>(param)->position);
            },
        };
        auto reader = std::unique_ptr<mov_reader_t, decltype(&mov_reader_destroy)>(mov_reader_create(&buffer, &memory), &mov_reader_destroy);
        require(reader != nullptr, "hls av1 mov reader create");

        struct track_capture
        {
            std::uint32_t video_track{};
            std::uint32_t audio_track{};
            bool av1{};
            bool aac{};
            int width{};
            int height{};
            std::vector<std::uint8_t> extra;
        } capture;
        mov_reader_trackinfo_t callbacks{
            .onvideo = [](void* param, std::uint32_t track, std::uint8_t object, int width, int height, const void* extra, std::size_t bytes) {
                auto& value = *static_cast<track_capture*>(param);
                value.video_track = track;
                value.av1 = object == MOV_OBJECT_AV1;
                value.width = width;
                value.height = height;
                const auto* begin = static_cast<const std::uint8_t*>(extra);
                value.extra.assign(begin, begin + bytes);
            },
            .onaudio = [](void* param, std::uint32_t track, std::uint8_t object, int, int, int, const void*, std::size_t) {
                auto& value = *static_cast<track_capture*>(param);
                value.audio_track = track;
                value.aac = object == MOV_OBJECT_AAC;
            },
            .onsubtitle = nullptr,
        };
        require(mov_reader_getinfo(reader.get(), &callbacks, &capture) == 0, "hls av1 mov track info");
        require(capture.av1 && capture.video_track != 0U && capture.width == source.width && capture.height == source.height && !capture.extra.empty(),
                "hls av1 mov video track");
        require(input_codec != codec_id::h264 || (capture.aac && capture.audio_track != 0U), "hls av1 mov aac track");
        aom_av1_t av1{};
        require(aom_av1_codec_configuration_record_load(capture.extra.data(), capture.extra.size(), &av1) == static_cast<int>(capture.extra.size()) &&
                    av1.bytes > 0,
                "hls av1 av1c");
        aom_av1_t sequence{};
        require(aom_av1_codec_configuration_record_init(&sequence, av1.data, av1.bytes) == 0 && sequence.width > 0 && sequence.height > 0 &&
                    sequence.seq_profile == av1.seq_profile && sequence.seq_level_idx_0 == av1.seq_level_idx_0 &&
                    sequence.seq_tier_0 == av1.seq_tier_0,
                "hls av1 av1c stream properties");

        struct sample_capture
        {
            std::uint32_t video_track{};
            std::uint32_t audio_track{};
            int video_samples{};
            int audio_samples{};
            bool key_frame{};
        } samples{.video_track = capture.video_track, .audio_track = capture.audio_track};
        std::vector<std::uint8_t> sample_buffer(1U << 20U);
        while (true)
        {
            const auto result = mov_reader_read(
                reader.get(),
                sample_buffer.data(),
                sample_buffer.size(),
                [](void* param, std::uint32_t track, const void*, std::size_t bytes, std::int64_t, std::int64_t, int flags) {
                    auto& value = *static_cast<sample_capture*>(param);
                    if (bytes == 0U)
                    {
                        return;
                    }
                    if (track == value.video_track)
                    {
                        ++value.video_samples;
                        value.key_frame = value.key_frame || (flags & MOV_AV_FLAG_KEYFREAME) != 0;
                    }
                    else if (track == value.audio_track)
                    {
                        ++value.audio_samples;
                    }
                },
                &samples);
            if (result == 0)
            {
                break;
            }
            require(result > 0, "hls av1 mov sample read");
        }
        require(samples.video_samples > 0 && samples.key_frame, "hls av1 fragmented mp4 video samples");
        require(input_codec != codec_id::h264 || samples.audio_samples > 0, "hls av1 fragmented mp4 aac samples");
    }

    const auto source = make_video_transcoder_fixture(codec_id::h264);
    hls_output reconfigured(hls_config{
        .target_duration_seconds = 1.0,
        .window_size = 4,
        .video = output_video_config{
            .codec = output_video_codec::av1,
        },
    });
    media_track video_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h264,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = source.codec_config,
        .config_version = 1,
    };
    reconfigured.on_track(video_track);
    for (const auto& frame : source.frames)
    {
        reconfigured.on_frame(frame);
    }
    video_track.config_version = 2;
    reconfigured.on_track(video_track);
    for (auto frame : source.frames)
    {
        frame.pts_ns += 1'000'000'000;
        frame.dts_ns += 1'000'000'000;
        reconfigured.on_frame(std::move(frame));
    }
    reconfigured.on_end();
    const auto reconfigured_playlist = reconfigured.playlist("/hls/av1-reconfigured");
    require(reconfigured_playlist.find("#EXT-X-MAP:URI=\"/hls/av1-reconfigured/init.mp4?v=1\"") != std::string::npos &&
                reconfigured_playlist.find("/hls/av1-reconfigured/1.m4s") != std::string::npos,
            "hls av1 config change versions init segment uri");
}

void test_hls_g711_output()
{
    for (const auto codec : {codec_id::g711a, codec_id::g711u})
    {
        hls_output output(hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});
        output.on_track(make_video_track());
        output.on_track(make_g711_track(codec));
        output.on_frame(make_video_frame(0, true));
        const auto payload = std::make_shared<const std::vector<std::uint8_t>>(160, codec == codec_id::g711a ? 0xd5 : 0xff);
        output.on_frame(media_frame{.track = audio_track_id, .dts_ns = 20'000'000, .pts_ns = 20'000'000, .key_frame = false, .payload = payload});
        output.on_frame(make_video_frame(1'000'000'000, true));
        output.on_end();
        require(output.segment_count() >= 1U && output.playlist(".").find("#EXTM3U") != std::string::npos, "hls g711 segment lifecycle");
        const auto segment = output.segment(0);
        require(segment.has_value(), "hls g711 segment");
        const auto capture = demux_ts_segment(*segment);
        const auto stream_type = codec == codec_id::g711a ? PSI_STREAM_AUDIO_G711A : PSI_STREAM_AUDIO_G711U;
        require(std::ranges::find(capture.stream_codecs, stream_type) != capture.stream_codecs.end(), "hls g711 pmt stream type");
        const auto packet = std::ranges::find_if(capture.packets, [stream_type](const demuxed_packet& value) { return value.codec == stream_type; });
        require(packet != capture.packets.end() && packet->payload == *payload, "hls g711 pes payload");
    }
}

void test_hls_service_lifecycle()
{
    boost::asio::io_context io;
    stream_registry registry;
    hls_service hls(registry, hls_config{.target_duration_seconds = 1.0, .window_size = 4, .video = {}});

    auto first = std::make_shared<media_stream>("live/hls", io.get_executor());
    require(first->set_tracks({make_video_track()}), "hls first track");
    require(registry.add(first), "hls first stream add");
    boost::asio::post(io,
                      [&]()
                      {
                          require(hls.segment_count("live/hls") == 0U, "hls first output create");
                          first->publish(make_video_frame(0, true));
                          first->publish(make_video_frame(1'000'000'000, true));
                      });
    io.run();

    registry.remove(*first);
    const auto detached_playlist = hls.playlist("live/hls");
    require(detached_playlist.has_value(), "hls output survives registry removal before source end");

    io.restart();
    boost::asio::post(io, [first]() { first->end(); });
    io.run();

    const auto ended_playlist = hls.playlist("live/hls");
    require(ended_playlist.has_value(), "hls ended playlist retained");
    require(ended_playlist->find("#EXT-X-ENDLIST") != std::string::npos, "hls ended playlist marker");
    const auto ended_segment = hls.segment("live/hls", 0);
    require(ended_segment.has_value() && !ended_segment->empty(), "hls ended segment retained");

    io.restart();
    auto second = std::make_shared<media_stream>("live/hls", io.get_executor());
    require(second->set_tracks({make_video_track()}), "hls replacement track");
    require(registry.add(second), "hls replacement stream add");
    boost::asio::post(io,
                      [&]()
                      {
                          require(hls.segment_count("live/hls") == 0U, "hls replacement output reset");
                          require(!hls.segment("live/hls", 0).has_value(), "hls replacement drops old segment");
                      });
    io.run();

    io.restart();
    auto overlap_first = std::make_shared<media_stream>("live/hls-overlap", io.get_executor());
    require(overlap_first->set_tracks({make_video_track()}), "hls overlap first track");
    require(registry.add(overlap_first), "hls overlap first stream add");
    boost::asio::post(io,
                      [&]()
                      {
                          require(hls.segment_count("live/hls-overlap") == 0U, "hls overlap first output create");
                          overlap_first->publish(make_video_frame(0, true));
                          overlap_first->publish(make_video_frame(1'000'000'000, true));
                      });
    io.run();

    registry.remove(*overlap_first);
    auto overlap_second = std::make_shared<media_stream>("live/hls-overlap", io.get_executor());
    require(overlap_second->set_tracks({make_video_track()}), "hls overlap replacement track");
    require(registry.add(overlap_second), "hls overlap replacement stream add");
    io.restart();
    boost::asio::post(io,
                      [&]()
                      {
                          require(hls.segment_count("live/hls-overlap") == 0U, "hls overlap replacement output create");
                          overlap_first->end();
                      });
    io.run();

    const auto overlap_playlist = hls.playlist("live/hls-overlap");
    require(overlap_playlist.has_value(), "hls overlap replacement playlist available");
    require(overlap_playlist->find("#EXT-X-ENDLIST") == std::string::npos, "hls old generation end does not end replacement");
    require(!hls.segment("live/hls-overlap", 0).has_value(), "hls overlap replacement does not expose old segment");

    io.restart();
    auto late = std::make_shared<media_stream>("live/hls-late", io.get_executor());
    require(late->set_tracks({make_video_track()}), "hls late track");
    require(registry.add(late), "hls late stream add");
    boost::asio::post(io,
                      [&]()
                      {
                          late->publish(make_video_frame(0, true));
                          late->publish(make_video_frame(500'000'000, false));
                          require(hls.segment_count("live/hls-late") == 0U, "hls late output replays current gop");
                          late->publish(make_video_frame(1'000'000'000, true));
                          require(hls.segment_count("live/hls-late") == 1U, "hls late replay participates in first segment");
                          registry.remove(*late);
                          late->end();
                      });
    io.run();
    const auto late_playlist = hls.playlist("live/hls-late");
    require(late_playlist.has_value() && late_playlist->find("#EXT-X-ENDLIST") != std::string::npos, "hls late output finalizes");

    stream_registry expiring_registry;
    hls_service expiring_hls(expiring_registry, hls_config{.target_duration_seconds = 0.001, .window_size = 1, .video = {}});
    io.restart();
    auto expiring_stream = std::make_shared<media_stream>("live/expiring", io.get_executor());
    require(expiring_stream->set_tracks({make_video_track()}), "hls expiring track");
    require(expiring_registry.add(expiring_stream), "hls expiring stream add");
    boost::asio::post(io,
                      [&]()
                      {
                          require(expiring_hls.segment_count("live/expiring") == 0U, "hls expiring output create");
                          expiring_registry.remove(*expiring_stream);
                          expiring_stream->end();
                      });
    io.run();
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

std::span<const std::uint8_t> require_rtp_mid(const std::vector<std::uint8_t>& packet, std::string_view expected_mid, int expected_id)
{
    rtp_packet_t parsed{};
    require(rtp_packet_deserialize(&parsed, packet.data(), static_cast<int>(packet.size())) == 0, "rtp mid deserialize");
    require(parsed.rtp.x == 1 && parsed.extension != nullptr && parsed.extlen > 0, "rtp mid extension present");

    const auto extension =
        std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(parsed.extension), static_cast<std::size_t>(parsed.extlen));
    std::size_t offset = 0;
    std::size_t length = 0;
    int extension_id = 0;
    if (parsed.extprofile == RTP_HDREXT_PROFILE_ONE_BYTE)
    {
        require(!extension.empty(), "rtp mid one byte header");
        extension_id = extension[0] >> 4U;
        length = (extension[0] & 0x0fU) + 1U;
        offset = 1;
    }
    else
    {
        require((parsed.extprofile & RTP_HDREXT_PROFILE_TWO_BYTE_FILTER) == RTP_HDREXT_PROFILE_TWO_BYTE && extension.size() >= 2U,
                "rtp mid two byte header");
        extension_id = extension[0];
        length = extension[1];
        offset = 2;
    }
    require(extension_id == expected_id && offset + length <= extension.size(), "rtp mid id and length");
    const auto mid = std::string_view(reinterpret_cast<const char*>(extension.data() + offset), length);
    require(mid == expected_mid, "rtp mid value");
    return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(parsed.payload), static_cast<std::size_t>(parsed.payloadlen));
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
    webrtc_output output(
        webrtc_output_config{.video_payload_type = 102, .video_mid = "video", .video_mid_extension_id = 20, .rtcp_cname = {}},
                         [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
    output.on_track(make_video_track());
    require(output.valid(), "webrtc video output valid");
    output.on_frame(make_video_frame(-40'000'000, false));
    require(packets.empty(), "webrtc waits natural key frame");
    output.on_frame(make_video_frame(0, true));

    require(!packets.empty() && packets.front().size() >= 12, "rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "rtp version 2");
    require((packets.front()[1] & 0x7fU) == 102U, "rtp negotiated payload type");
    require(!require_rtp_mid(packets.front(), "video", 20).empty(), "rtp video mid payload");

    const auto first_timestamp = rtp_timestamp(packets.front());
    const auto first_frame_packet_count = packets.size();
    output.on_frame(make_video_frame(40'000'000, false));
    require(packets.size() > first_frame_packet_count, "webrtc second h264 frame packetized");
    require(rtp_timestamp(packets.back()) - first_timestamp == 3'600U, "h264 rtp timestamp step");
}

void test_webrtc_video_access_unit_marker()
{
    for (const bool h265 : std::array{false, true})
    {
        std::vector<std::vector<std::uint8_t>> packets;
        webrtc_output output(
            webrtc_output_config{
                .video_codec = h265 ? codec_id::h265 : codec_id::h264,
                .video_payload_type = 102,
                .video_mid = "video",
                .video_mid_extension_id = 4,
                .rtcp_cname = {},
            },
            [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
        output.on_track(h265 ? make_h265_track() : make_video_track());
        require(output.valid(), "webrtc marker output valid");

        auto frame = h265 ? make_h265_frame(0, true) : make_video_frame(0, true);
        auto payload = std::make_shared<std::vector<std::uint8_t>>(*frame.payload);
        if (h265)
        {
            const std::array<std::uint8_t, 9> suffix_sei{0, 0, 0, 1, 0x50, 0x01, 0x05, 0x01, 0x80};
            payload->insert(payload->end(), suffix_sei.begin(), suffix_sei.end());
        }
        else
        {
            const std::array<std::uint8_t, 8> sei{0, 0, 0, 1, 0x06, 0x05, 0x01, 0x80};
            payload->insert(payload->end(), sei.begin(), sei.end());
        }
        frame.payload = std::move(payload);
        output.on_frame(frame);

        require(packets.size() >= 2U, "webrtc marker multiple nalu packets");
        const auto marker_count =
            std::count_if(packets.begin(), packets.end(), [](const auto& packet) { return (packet[1] & 0x80U) != 0; });
        require(marker_count == 1 && (packets.back()[1] & 0x80U) != 0, "webrtc marker on access unit last packet");
    }
}

void test_webrtc_av1_packetizer()
{
    for (const auto input_codec : {codec_id::h264, codec_id::h265})
    {
        const auto source = make_video_transcoder_fixture(input_codec);
        std::vector<std::vector<std::uint8_t>> packets;
        webrtc_output output(
            webrtc_output_config{
                .video_codec = codec_id::av1,
                .video_payload_type = 99,
                .video_mid = "video",
                .video_mid_extension_id = 4,
                .rtcp_cname = {},
            },
            [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
        output.on_track(media_track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = input_codec,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = source.codec_config,
        });
        require(output.valid(), "webrtc av1 output valid");
        for (const auto& frame : source.frames)
        {
            output.on_frame(frame);
        }
        require(!packets.empty(), "webrtc av1 packets");
        require(std::ranges::all_of(packets, [](const auto& packet) { return packet.size() >= 12U && (packet[1] & 0x7fU) == 99U; }),
                "webrtc av1 payload type");
        require(!require_rtp_mid(packets.front(), "video", 4).empty(), "webrtc av1 mid");
        require(std::ranges::any_of(packets, [](const auto& packet) { return (packet[1] & 0x80U) != 0; }),
                "webrtc av1 marker");
    }
}

void test_webrtc_opus_channel_count(int channel_count, int bitrate = -1, int max_playback_rate = 48'000)
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{
            .audio_payload_type = 111,
            .opus_channel_count = channel_count,
            .opus_bitrate = bitrate,
            .opus_max_playback_rate = max_playback_rate,
            .audio_mid = "1",
            .audio_mid_extension_id = 4,
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
    require(!require_rtp_mid(packets.front(), "1", 4).empty(), "rtp audio mid payload");

    if (packets.size() >= 2U)
    {
        require(rtp_timestamp(packets[1]) - rtp_timestamp(packets[0]) == 960U, "opus rtp timestamp step");
    }
}

void test_webrtc_opus_passthrough()
{
    constexpr std::string_view cname = "webrtc-opus-passthrough";
    constexpr std::string_view mid = "audio-mid";
    constexpr int mid_extension_id = 20;
    std::vector<std::vector<std::uint8_t>> rtp_packets;
    std::vector<std::vector<std::uint8_t>> rtcp_packets;
    webrtc_output output(
        webrtc_output_config{
            .audio_codec = codec_id::opus,
            .audio_payload_type = 109,
            .opus_channel_count = 2,
            .audio_mid = std::string(mid),
            .audio_mid_extension_id = mid_extension_id,
            .rtcp_cname = std::string(cname),
        },
        [&rtp_packets](std::span<const std::uint8_t> packet) { rtp_packets.emplace_back(packet.begin(), packet.end()); },
        [&rtcp_packets](std::span<const std::uint8_t> packet) { rtcp_packets.emplace_back(packet.begin(), packet.end()); });
    output.on_track(make_opus_track(2));
    require(output.valid(), "webrtc opus passthrough output valid");

    const std::array<std::vector<std::uint8_t>, 3> payloads{
        std::vector<std::uint8_t>{0xf8, 0xff, 0xfe},
        std::vector<std::uint8_t>{0x78, 0x11, 0x22, 0x33},
        std::vector<std::uint8_t>{0x48, 0x44, 0x55},
    };
    for (std::size_t index = 0; index < payloads.size(); ++index)
    {
        output.on_frame(make_opus_frame(static_cast<std::int64_t>(index) * 20'000'000, payloads[index]));
    }

    require(rtp_packets.size() == payloads.size(), "webrtc opus passthrough packet count");
    for (std::size_t index = 0; index < rtp_packets.size(); ++index)
    {
        require((rtp_packets[index][1] & 0x7fU) == 109U, "webrtc opus passthrough negotiated payload type");
        const auto payload = require_rtp_mid(rtp_packets[index], mid, mid_extension_id);
        require(std::ranges::equal(payload, payloads[index]), "webrtc opus passthrough raw payload");
        if (index > 0)
        {
            require(rtp_timestamp(rtp_packets[index]) - rtp_timestamp(rtp_packets[index - 1U]) == 960U,
                    "webrtc opus passthrough timestamp step");
        }
    }
    require(!rtcp_packets.empty(), "webrtc opus passthrough rtcp report");
    require(require_rtcp_sender_report(rtcp_packets.back(), cname) == rtp_ssrc(rtp_packets.back()),
            "webrtc opus passthrough rtcp sender state");

    const auto packet_count = rtp_packets.size();
    output.on_frame(make_opus_frame(60'000'001));
    require(rtp_packets.size() == packet_count, "webrtc opus passthrough rejects fractional millisecond");

    const auto extension_data_size = (2U + mid.size() + 3U) & ~std::size_t{3U};
    const auto payload_capacity = static_cast<std::size_t>(rtp_packet_getsize() - RTP_FIXED_HEADER) - 4U - extension_data_size;
    const std::vector<std::uint8_t> maximum_payload(payload_capacity, 0x55);
    output.on_frame(make_opus_frame(80'000'000, maximum_payload));
    require(rtp_packets.size() == packet_count + 1U && rtp_packets.back().size() == static_cast<std::size_t>(rtp_packet_getsize()),
            "webrtc opus passthrough mid adjusted capacity");
    require(std::ranges::equal(require_rtp_mid(rtp_packets.back(), mid, mid_extension_id), maximum_payload),
            "webrtc opus passthrough maximum raw payload");

    output.on_frame(make_opus_frame(100'000'000, std::vector<std::uint8_t>(payload_capacity + 1U, 0x66)));
    require(rtp_packets.size() == packet_count + 1U, "webrtc opus passthrough rejects oversized packet");
}

void test_webrtc_g711_passthrough_case(codec_id codec)
{
    require(codec == codec_id::g711a || codec == codec_id::g711u, "webrtc g711 passthrough codec");
    constexpr std::string_view mid = "g711-audio-mid";
    constexpr int extension_id = 20;
    const auto payload_type = codec == codec_id::g711a ? RTP_PAYLOAD_PCMA : RTP_PAYLOAD_PCMU;
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{
            .audio_codec = codec,
            .audio_payload_type = payload_type,
            .audio_mid = std::string(mid),
            .audio_mid_extension_id = extension_id,
            .rtcp_cname = {},
        },
        [&packets](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
    output.on_track(make_g711_track(codec));
    require(output.valid(), "webrtc g711 passthrough output valid");

    const std::array<std::vector<std::uint8_t>, 3> payloads{
        std::vector<std::uint8_t>(160, 0x11),
        std::vector<std::uint8_t>(160, 0x22),
        std::vector<std::uint8_t>(160, 0x33),
    };
    for (std::size_t index = 0; index < payloads.size(); ++index)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = static_cast<std::int64_t>(index) * 20'000'000,
            .pts_ns = static_cast<std::int64_t>(index) * 20'000'000,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(payloads[index]),
        });
    }
    require(packets.size() == payloads.size(), "webrtc g711 passthrough packet count");
    for (std::size_t index = 0; index < packets.size(); ++index)
    {
        require((packets[index][1] & 0x7fU) == static_cast<unsigned int>(payload_type), "webrtc g711 static payload type");
        require(std::ranges::equal(require_rtp_mid(packets[index], mid, extension_id), payloads[index]), "webrtc g711 raw payload");
        if (index > 0)
        {
            require(rtp_timestamp(packets[index]) - rtp_timestamp(packets[index - 1U]) == 160U, "webrtc g711 timestamp step");
        }
    }

    const auto packet_count = packets.size();
    output.on_frame(media_frame{
        .track = audio_track_id,
        .dts_ns = 60'000'001,
        .pts_ns = 60'000'001,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0x44),
    });
    require(packets.size() == packet_count, "webrtc g711 rejects fractional millisecond");

    const auto extension_data_size = (2U + mid.size() + 3U) & ~std::size_t{3U};
    const auto capacity = static_cast<std::size_t>(rtp_packet_getsize() - RTP_FIXED_HEADER) - 4U - extension_data_size;
    output.on_frame(media_frame{
        .track = audio_track_id,
        .dts_ns = 80'000'000,
        .pts_ns = 80'000'000,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(capacity, 0x55),
    });
    require(packets.size() == packet_count + 1U && packets.back().size() == static_cast<std::size_t>(rtp_packet_getsize()),
            "webrtc g711 mid adjusted capacity");
    output.on_frame(media_frame{
        .track = audio_track_id,
        .dts_ns = 100'000'000,
        .pts_ns = 100'000'000,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(capacity + 1U, 0x66),
    });
    require(packets.size() == packet_count + 1U, "webrtc g711 rejects oversized packet");
}

void test_webrtc_g711_passthrough()
{
    test_webrtc_g711_passthrough_case(codec_id::g711a);
    test_webrtc_g711_passthrough_case(codec_id::g711u);
}

void test_webrtc_output_initialization_failure()
{
    webrtc_output invalid_video(
        webrtc_output_config{.video_payload_type = 102, .video_mid = "0", .video_mid_extension_id = 4, .rtcp_cname = {}}, [](std::span<const std::uint8_t>) {});
    auto video = make_video_track();
    video.codec_config.clear();
    invalid_video.on_track(video);
    require(!invalid_video.valid(), "webrtc invalid h264 output rejected");

    webrtc_output invalid_audio(
        webrtc_output_config{.audio_payload_type = 111, .opus_channel_count = 3, .audio_mid = "1", .audio_mid_extension_id = 4, .rtcp_cname = {}},
                                [](std::span<const std::uint8_t>) {});
    invalid_audio.on_track(make_audio_track());
    require(!invalid_audio.valid(), "webrtc invalid opus output rejected");

    webrtc_output invalid_mid(
        webrtc_output_config{.video_payload_type = 102, .video_mid = "0123456789abcdef0", .video_mid_extension_id = 4, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    invalid_mid.on_track(make_video_track());
    require(!invalid_mid.valid(), "webrtc long mid output rejected");

    webrtc_output invalid_h264_payload(
        webrtc_output_config{.video_payload_type = 72, .video_mid = "0", .video_mid_extension_id = 4, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    invalid_h264_payload.on_track(make_video_track());
    require(!invalid_h264_payload.valid(), "webrtc rtcp mux h264 payload rejected");

    webrtc_output invalid_h265_payload(
        webrtc_output_config{.video_codec = codec_id::h265, .video_payload_type = 72, .video_mid = "0", .video_mid_extension_id = 4, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    invalid_h265_payload.on_track(make_h265_track());
    require(!invalid_h265_payload.valid(), "webrtc rtcp mux h265 payload rejected");

    webrtc_output invalid_opus_payload(
        webrtc_output_config{.audio_payload_type = 95, .opus_channel_count = 2, .audio_mid = "1", .audio_mid_extension_id = 4, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    invalid_opus_payload.on_track(make_audio_track());
    require(!invalid_opus_payload.valid(), "webrtc rtcp mux opus payload rejected");
}

void test_webrtc_rtcp_sender()
{
    constexpr std::string_view cname = "webrtc-test-cname";
    std::vector<std::vector<std::uint8_t>> rtp_packets;
    std::vector<std::vector<std::uint8_t>> rtcp_packets;
    webrtc_output output(
        webrtc_output_config{
            .video_payload_type = 102,
            .audio_payload_type = 111,
            .opus_channel_count = 2,
            .video_mid = "0",
            .audio_mid = "1",
            .video_mid_extension_id = 4,
            .audio_mid_extension_id = 4,
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
    test_webrtc_opus_channel_count(2, 32'000, 16'000);
    test_webrtc_opus_passthrough();
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_timebase_conversions();
    std::cout << "[pass] timebase_conversions\n";
    media_server::test_rtmp_legacy_fourcc_connect_parse();
    std::cout << "[pass] rtmp_legacy_fourcc_connect_parse\n";
    media_server::test_rtmp_timestamp_timeline();
    std::cout << "[pass] rtmp_timestamp_timeline\n";
    media_server::test_internal_format_contract();
    std::cout << "[pass] internal_format_contract\n";
    media_server::test_rtmp_aac_asc_adts_contract();
    std::cout << "[pass] rtmp_aac_asc_adts_contract\n";
    media_server::test_rtmp_input_initial_topology();
    std::cout << "[pass] rtmp_input_initial_topology\n";
    media_server::test_rtmp_input_initial_tracks_timeout();
    std::cout << "[pass] rtmp_input_initial_tracks_timeout\n";
    media_server::test_rtmp_input_codec_configuration_updates();
    std::cout << "[pass] rtmp_input_codec_configuration_updates\n";
    media_server::test_rtmp_input_rejects_video_codec_change();
    std::cout << "[pass] rtmp_input_rejects_video_codec_change\n";
    media_server::test_rtmp_input_rejects_audio_codec_change();
    std::cout << "[pass] rtmp_input_rejects_audio_codec_change\n";
    media_server::test_rtmp_rejects_live_playback_control();
    std::cout << "[pass] rtmp_rejects_live_playback_control\n";
    media_server::test_rtmp_output_pull_codecs_and_order();
    std::cout << "[pass] rtmp_output_pull_codecs_and_order\n";
    media_server::test_rtmp_output_config_reset_and_end();
    std::cout << "[pass] rtmp_output_config_reset_and_end\n";
    media_server::test_rtmp_tcp_error_lifecycle();
    std::cout << "[pass] rtmp_tcp_error_lifecycle\n";
    media_server::test_ireader_h266_avpacket_codec_identity();
    std::cout << "[pass] ireader_h266_avpacket_codec_identity\n";
    media_server::test_ireader_avs3_flv_mux_codec_identity();
    std::cout << "[pass] ireader_avs3_flv_mux_codec_identity\n";
    media_server::test_ireader_rejects_unknown_enhanced_audio_fourcc();
    std::cout << "[pass] ireader_rejects_unknown_enhanced_audio_fourcc\n";
    media_server::test_ireader_opus_flv_correctness();
    std::cout << "[pass] ireader_opus_flv_correctness\n";
    media_server::test_ireader_avpbs_opus_lifetime();
    std::cout << "[pass] ireader_avpbs_opus_lifetime\n";
    media_server::test_ireader_av1_packet_lifetime();
    std::cout << "[pass] ireader_av1_packet_lifetime\n";
    media_server::test_ireader_av1_marker_boundary();
    std::cout << "[pass] ireader_av1_marker_boundary\n";
    media_server::test_flv_config_cache_lifecycle();
    std::cout << "[pass] flv_config_cache_lifecycle\n";
    media_server::test_flv_av1_transcode_round_trip();
    std::cout << "[pass] flv_av1_transcode_round_trip\n";
    media_server::test_flv_g711_round_trip();
    std::cout << "[pass] flv_g711_round_trip\n";
    media_server::test_flv_opus_adapter_round_trip();
    std::cout << "[pass] flv_opus_adapter_round_trip\n";
    media_server::test_h265_output_paths();
    std::cout << "[pass] h265_output_paths\n";
    media_server::test_rtsp_muxer_zero_origin_timeline();
    std::cout << "[pass] rtsp_muxer_zero_origin_timeline\n";
    media_server::test_ireader_negotiated_payload_mapping();
    std::cout << "[pass] ireader_negotiated_payload_mapping\n";
    media_server::test_ireader_av1_sdp_payload_type();
    std::cout << "[pass] ireader_av1_sdp_payload_type\n";
    media_server::test_rtsp_aac_adts_round_trip();
    std::cout << "[pass] rtsp_aac_adts_round_trip\n";
    media_server::test_audio_transcoder_aac_opus();
    std::cout << "[pass] audio_transcoder_aac_opus\n";
    media_server::test_video_transcoder_h26x_av1();
    std::cout << "[pass] video_transcoder_h26x_av1\n";
    media_server::test_media_stream_configless_audio_track();
    std::cout << "[pass] media_stream_configless_audio_track\n";
    media_server::test_rtsp_client_session_timeout();
    std::cout << "[pass] rtsp_client_session_timeout\n";
    media_server::test_tcp_connection_shutdown_lifecycle();
    std::cout << "[pass] tcp_connection_shutdown_lifecycle\n";
    media_server::test_tcp_connection_io_error_propagation();
    std::cout << "[pass] tcp_connection_io_error_propagation\n";
    media_server::test_tcp_connection_shutdown_discards_pending_writes();
    std::cout << "[pass] tcp_connection_shutdown_discards_pending_writes\n";
    media_server::test_udp_socket_receive_and_send();
    std::cout << "[pass] udp_socket_receive_and_send\n";
    media_server::test_udp_socket_multi_endpoint_queue();
    std::cout << "[pass] udp_socket_multi_endpoint_queue\n";
    media_server::test_udp_socket_error_and_shutdown_lifecycle();
    std::cout << "[pass] udp_socket_error_and_shutdown_lifecycle\n";
    media_server::test_tcp_listener_startup_error();
    std::cout << "[pass] tcp_listener_startup_error\n";
    media_server::test_tcp_listener_worker_affinity();
    std::cout << "[pass] tcp_listener_worker_affinity\n";
    media_server::test_tcp_listener_shutdown_lifecycle();
    std::cout << "[pass] tcp_listener_shutdown_lifecycle\n";
    media_server::test_rtmp_server_shutdown_lifecycle();
    std::cout << "[pass] rtmp_server_shutdown_lifecycle\n";
    media_server::test_http_flv_client_disconnect();
    std::cout << "[pass] http_flv_client_disconnect\n";
    media_server::test_http_flv_stream_end_during_write();
    std::cout << "[pass] http_flv_stream_end_during_write\n";
    media_server::test_http_flv_pending_bootstrap_end();
    std::cout << "[pass] http_flv_pending_bootstrap_end\n";
    media_server::test_http_flv_batch_consumption_and_overrun();
    std::cout << "[pass] http_flv_batch_consumption_and_overrun\n";
    media_server::test_http_flv_h265_pull();
    std::cout << "[pass] http_flv_h265_pull\n";
    media_server::test_http_flv_fast_and_slow_readers();
    std::cout << "[pass] http_flv_fast_and_slow_readers\n";
    media_server::test_http_flv_audio_video_order();
    std::cout << "[pass] http_flv_audio_video_order\n";
    media_server::test_http_flv_config_reset();
    std::cout << "[pass] http_flv_config_reset\n";
    media_server::test_rtsp_pull_url_contract();
    std::cout << "[pass] rtsp_pull_url_contract\n";
    media_server::test_rtsp_input_establishment_timeout();
    std::cout << "[pass] rtsp_input_establishment_timeout\n";
    media_server::test_rtsp_input_establishment_progress_timeout();
    std::cout << "[pass] rtsp_input_establishment_progress_timeout\n";
    media_server::test_rtsp_input_selects_single_audio_and_video();
    std::cout << "[pass] rtsp_input_selects_single_audio_and_video\n";
    media_server::test_rtsp_input_opus_passthrough();
    std::cout << "[pass] rtsp_input_opus_passthrough\n";
    media_server::test_rtsp_input_rejects_invalid_opus_rate();
    std::cout << "[pass] rtsp_input_rejects_invalid_opus_rate\n";
    media_server::test_rtsp_input_g711_passthrough();
    std::cout << "[pass] rtsp_input_g711_passthrough\n";
    media_server::test_rtsp_input_rejects_mismatched_g711_rtpmap();
    std::cout << "[pass] rtsp_input_rejects_mismatched_g711_rtpmap\n";
    media_server::test_rtsp_input_rejects_audio_only_source();
    std::cout << "[pass] rtsp_input_rejects_audio_only_source\n";
    media_server::test_rtsp_input_uses_complete_sdp_topology_without_track_wait();
    std::cout << "[pass] rtsp_input_uses_complete_sdp_topology_without_track_wait\n";
    media_server::test_rtsp_input_initial_tracks_timeout();
    std::cout << "[pass] rtsp_input_initial_tracks_timeout\n";
    media_server::test_rtsp_input_independent_keepalive();
    std::cout << "[pass] rtsp_input_independent_keepalive\n";
    media_server::test_rtsp_client_rejects_empty_media_selection();
    std::cout << "[pass] rtsp_client_rejects_empty_media_selection\n";
    media_server::test_rtsp_publish_server_contract();
    std::cout << "[pass] rtsp_publish_server_contract\n";
    media_server::test_rtsp_output_session_contract();
    std::cout << "[pass] rtsp_output_session_contract\n";
    media_server::test_rtsp_output_pull_media();
    std::cout << "[pass] rtsp_output_pull_media\n";
    media_server::test_rtsp_output_audio_video_order();
    std::cout << "[pass] rtsp_output_audio_video_order\n";
    media_server::test_rtsp_output_h265();
    std::cout << "[pass] rtsp_output_h265\n";
    media_server::test_rtsp_output_av1();
    std::cout << "[pass] rtsp_output_av1\n";
    media_server::test_rtsp_output_opus_passthrough_boundaries();
    std::cout << "[pass] rtsp_output_opus_passthrough_boundaries\n";
    media_server::test_rtsp_output_g711_passthrough();
    std::cout << "[pass] rtsp_output_g711_passthrough\n";
    media_server::test_rtsp_output_setup_track_lifecycle();
    std::cout << "[pass] rtsp_output_setup_track_lifecycle\n";
    media_server::test_rtsp_output_unsetup_audio_update_keeps_video_continuity();
    std::cout << "[pass] rtsp_output_unsetup_audio_update_keeps_video_continuity\n";
    media_server::test_rtsp_output_rejects_stale_description();
    std::cout << "[pass] rtsp_output_rejects_stale_description\n";
    media_server::test_media_stream_sink_lifecycle();
    std::cout << "[pass] media_stream_sink_lifecycle\n";
    media_server::test_media_stream_sink_gop_replay();
    std::cout << "[pass] media_stream_sink_gop_replay\n";
    media_server::test_media_stream_sink_owner_affinity();
    std::cout << "[pass] media_stream_sink_owner_affinity\n";
    media_server::test_media_stream_pull_reader_overrun();
    std::cout << "[pass] media_stream_pull_reader_overrun\n";
    media_server::test_media_stream_pull_reader_duplicate_read();
    std::cout << "[pass] media_stream_pull_reader_duplicate_read\n";
    media_server::test_media_stream_pull_reader_batch_limit_and_worker_filtering();
    std::cout << "[pass] media_stream_pull_reader_batch_limit_and_worker_filtering\n";
    media_server::test_media_stream_pull_reader_initial_cursor_starts_current_gop();
    std::cout << "[pass] media_stream_pull_reader_initial_cursor_starts_current_gop\n";
    media_server::test_media_stream_pull_reader_previous_gop_continuity();
    std::cout << "[pass] media_stream_pull_reader_previous_gop_continuity\n";
    media_server::test_media_stream_add_reader_after_end();
    std::cout << "[pass] media_stream_add_reader_after_end\n";
    media_server::test_media_stream_pull_reader_track_snapshot_order();
    std::cout << "[pass] media_stream_pull_reader_track_snapshot_order\n";
    media_server::test_media_stream_reader_track_interest();
    std::cout << "[pass] media_stream_reader_track_interest\n";
    media_server::test_media_stream_video_keyframe_barrier_is_sticky();
    std::cout << "[pass] media_stream_video_keyframe_barrier_is_sticky\n";
    media_server::test_stream_registry_generation_lifecycle();
    std::cout << "[pass] stream_registry_generation_lifecycle\n";
    media_server::test_hls_output();
    std::cout << "[pass] hls_output\n";
    media_server::test_hls_av1_fmp4_output();
    std::cout << "[pass] hls_av1_fmp4_output\n";
    media_server::test_hls_g711_output();
    std::cout << "[pass] hls_g711_output\n";
    media_server::test_hls_service_lifecycle();
    std::cout << "[pass] hls_service_lifecycle\n";
    media_server::test_webrtc_rtp_packetizer();
    std::cout << "[pass] webrtc_rtp_packetizer\n";
    media_server::test_webrtc_video_access_unit_marker();
    std::cout << "[pass] webrtc_video_access_unit_marker\n";
    media_server::test_webrtc_av1_packetizer();
    std::cout << "[pass] webrtc_av1_packetizer\n";
    media_server::test_webrtc_opus_packetizer();
    std::cout << "[pass] webrtc_opus_packetizer\n";
    media_server::test_webrtc_g711_passthrough();
    std::cout << "[pass] webrtc_g711_passthrough\n";
    media_server::test_webrtc_output_initialization_failure();
    std::cout << "[pass] webrtc_output_initialization_failure\n";
    media_server::test_webrtc_rtcp_sender();
    std::cout << "[pass] webrtc_rtcp_sender\n";
    std::cout << "all tests passed\n";
    return 0;
}
