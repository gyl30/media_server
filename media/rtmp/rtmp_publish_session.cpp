#include <array>
#include <algorithm>
#include <vector>
#include <utility>
#include <string_view>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtmp/rtmp_publish_session.h"

extern "C"
{
#include "amf0.h"
#include "flv-proto.h"
#include "opus-head.h"
#include "flv-demuxer.h"
}

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
}    // namespace

rtmp_publish_session::rtmp_publish_session(worker_context& worker, std::string stream_name, shutdown_handler on_shutdown)
    : worker_(worker), stream_(std::make_shared<media_stream>(std::move(stream_name), worker_)), shutdown_handler_(std::move(on_shutdown))
{
}

bool rtmp_publish_session::startup()
{
    demuxer_ = flv_demuxer_create(&rtmp_publish_session::demux_callback, this);
    if (demuxer_ == nullptr)
    {
        return false;
    }

    return true;
}

void rtmp_publish_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_publish_session::safe_shutdown()
{
    shutdown_handler_ = {};
    if (stream_)
    {
        stream_registry::instance().remove(*stream_);
        stream_->end();
        stream_.reset();
    }
    if (demuxer_ != nullptr)
    {
        flv_demuxer_destroy(demuxer_);
        demuxer_ = nullptr;
    }
}

int rtmp_publish_session::on_video(const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    if (!shutdown_handler_ || demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(demuxer_, FLV_TYPE_VIDEO, data, bytes, timestamp);
}

int rtmp_publish_session::on_audio(const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    if (!shutdown_handler_ || demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(demuxer_, FLV_TYPE_AUDIO, data, bytes, timestamp);
}

int rtmp_publish_session::on_script(std::span<const std::uint8_t> data)
{
    if (!shutdown_handler_)
    {
        return -1;
    }
    if (data.empty())
    {
        return 0;
    }

    const auto* end = data.data() + data.size();
    std::array<char, 16> name{};
    if (data.front() != AMF_STRING)
    {
        return 0;
    }
    const auto* values = AMFReadString(data.data() + 1, end, 0, name.data(), name.size());
    if (values == nullptr)
    {
        return -1;
    }
    if (std::string_view(name.data()) != "onMetaData")
    {
        return 0;
    }

    double audio_codec{};
    std::array<amf_object_item_t, 1> properties{
        amf_object_item_t{AMF_NUMBER, "audiocodecid", &audio_codec, sizeof(audio_codec)},
    };
    std::array<amf_object_item_t, 1> metadata{
        amf_object_item_t{AMF_OBJECT, "metadata", properties.data(), properties.size()},
    };
    if (amf_read_items(values, end, metadata.data(), metadata.size()) == nullptr)
    {
        return -1;
    }

    const bool audio = audio_codec != 0.0;
    if ((expected_audio_.has_value() && *expected_audio_ != audio) || (initial_audio_track_ && !audio))
    {
        return -1;
    }

    expected_audio_ = audio;
    return register_stream_if_ready();
}

int rtmp_publish_session::demux_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (data == nullptr)
    {
        return -1;
    }
    return static_cast<rtmp_publish_session*>(param)->on_flv_demux(
        codec, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes), pts, dts, flags);
}

int rtmp_publish_session::handle_video_config(int codec, std::span<const std::uint8_t> data)
{
    const auto video_codec = codec == FLV_VIDEO_AVCC ? codec_id::h264 : codec_id::h265;
    auto config = codec == FLV_VIDEO_AVCC ? h264_avcc_to_annex_b(data) : h265_hvcc_to_annex_b(data);
    if (config.empty())
    {
        return -1;
    }

    media_track track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = video_codec,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = std::move(config),
    };
    if (stream_->tracks().empty())
    {
        if (initial_video_track_ && initial_video_track_->codec != video_codec)
        {
            spdlog::warn("rtmp publish video codec change {} {}", to_string(initial_video_track_->codec), to_string(video_codec));
            return -1;
        }
        initial_video_track_ = std::move(track);
        return register_stream_if_ready();
    }
    const auto fixed = std::ranges::find_if(stream_->tracks(), [](const media_track& value) { return value.id == video_track_id; });
    if (fixed == stream_->tracks().end() || fixed->codec != track.codec || fixed->clock_rate != track.clock_rate ||
        fixed->channel_count != track.channel_count || fixed->codec_config != track.codec_config)
    {
        spdlog::warn("rtmp publish video config changed {}", to_string(video_codec));
        return -1;
    }
    return 0;
}

int rtmp_publish_session::handle_audio_config(int codec, std::span<const std::uint8_t> data)
{
    if (expected_audio_.has_value() && !*expected_audio_)
    {
        return -1;
    }
    const auto audio_codec = codec == FLV_AUDIO_ASC ? codec_id::aac : codec_id::opus;
    if (codec == FLV_AUDIO_ASC)
    {
        const auto config = parse_aac_asc(data);
        if (!config)
        {
            return -1;
        }

        media_track track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = codec_id::aac,
            .clock_rate = config->sample_rate,
            .channel_count = config->channel_count,
            .codec_config = {data.begin(), data.end()},
        };
        if (stream_->tracks().empty())
        {
            if (initial_audio_track_ && initial_audio_track_->codec != audio_codec)
            {
                spdlog::warn("rtmp publish audio codec change {} {}", to_string(initial_audio_track_->codec), to_string(audio_codec));
                return -1;
            }
            initial_audio_track_ = std::move(track);
            return register_stream_if_ready();
        }
        const auto fixed = std::ranges::find_if(stream_->tracks(), [](const media_track& value) { return value.id == audio_track_id; });
        if (fixed == stream_->tracks().end() || fixed->codec != track.codec || fixed->clock_rate != track.clock_rate ||
            fixed->channel_count != track.channel_count || fixed->codec_config != track.codec_config)
        {
            spdlog::warn("rtmp publish audio config changed aac sample_rate {} channels {}", config->sample_rate, config->channel_count);
            return -1;
        }
        return 0;
    }

    opus_head_t head{};
    if (opus_head_load(data.data(), data.size(), &head) < 0 || (opus_head_channels(&head) != 1 && opus_head_channels(&head) != 2))
    {
        return -1;
    }

    media_track track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::opus,
        .clock_rate = 48'000,
        .channel_count = static_cast<std::uint16_t>(opus_head_channels(&head)),
        .codec_config = {},
    };
    if (stream_->tracks().empty())
    {
        if (initial_audio_track_ && initial_audio_track_->codec != audio_codec)
        {
            spdlog::warn("rtmp publish audio codec change {} {}", to_string(initial_audio_track_->codec), to_string(audio_codec));
            return -1;
        }
        initial_audio_track_ = std::move(track);
        return register_stream_if_ready();
    }
    const auto fixed = std::ranges::find_if(stream_->tracks(), [](const media_track& value) { return value.id == audio_track_id; });
    if (fixed == stream_->tracks().end() || fixed->codec != track.codec || fixed->clock_rate != track.clock_rate ||
        fixed->channel_count != track.channel_count || fixed->codec_config != track.codec_config)
    {
        spdlog::warn("rtmp publish audio config changed opus");
        return -1;
    }
    return 0;
}

int rtmp_publish_session::initialize_g711_track(int codec)
{
    if (expected_audio_.has_value() && !*expected_audio_)
    {
        return -1;
    }
    const auto audio_codec = codec == FLV_AUDIO_G711A ? codec_id::g711a : codec_id::g711u;
    if (stream_->tracks().empty())
    {
        if (initial_audio_track_ && initial_audio_track_->codec != audio_codec)
        {
            spdlog::warn("rtmp publish audio codec change {} {}", to_string(initial_audio_track_->codec), to_string(audio_codec));
            return -1;
        }
        initial_audio_track_ = media_track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = audio_codec,
            .clock_rate = 8'000,
            .channel_count = 1,
            .codec_config = {},
        };
        return register_stream_if_ready();
    }
    const auto fixed = std::ranges::find_if(stream_->tracks(), [](const media_track& value) { return value.id == audio_track_id; });
    return fixed != stream_->tracks().end() && fixed->codec == audio_codec ? 0 : -1;
}

int rtmp_publish_session::publish_media(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags)
{
    track_id id{};
    if (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265)
    {
        id = video_track_id;
    }
    else if (codec == FLV_AUDIO_AAC || codec == FLV_AUDIO_OPUS || codec == FLV_AUDIO_G711A || codec == FLV_AUDIO_G711U)
    {
        id = audio_track_id;
    }
    else
    {
        return 0;
    }

    const auto incoming_codec = codec == FLV_VIDEO_H264    ? codec_id::h264
                                : codec == FLV_VIDEO_H265  ? codec_id::h265
                                : codec == FLV_AUDIO_AAC   ? codec_id::aac
                                : codec == FLV_AUDIO_OPUS  ? codec_id::opus
                                : codec == FLV_AUDIO_G711A ? codec_id::g711a
                                                           : codec_id::g711u;
    if (stream_->tracks().empty())
    {
        const auto& pending = id == video_track_id ? initial_video_track_ : initial_audio_track_;
        if (pending && pending->codec != incoming_codec)
        {
            spdlog::warn("rtmp publish raw codec change {} {}", to_string(pending->codec), to_string(incoming_codec));
            return -1;
        }
        return 0;
    }
    const auto fixed = std::ranges::find_if(stream_->tracks(), [id](const media_track& value) { return value.id == id; });
    if (fixed == stream_->tracks().end() || fixed->codec != incoming_codec)
    {
        if (fixed != stream_->tracks().end())
        {
            spdlog::warn("rtmp publish raw codec change {} {}", to_string(fixed->codec), to_string(incoming_codec));
        }
        return -1;
    }

    const auto dts_ms = unwrap_rtmp_timestamp(dts, timestamp_);
    const auto pts_ms = dts_ms + rtmp_timestamp_delta(pts, dts);

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(data.begin(), data.end());
    media_frame frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(dts_ms),
        .pts_ns = milliseconds_to_ns(pts_ms),
        .key_frame = (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265) && flags != 0,
        .payload = std::move(payload),
    };
    stream_->publish(std::move(frame));
    return 0;
}

int rtmp_publish_session::on_flv_demux(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (!stream_)
    {
        return -1;
    }

    if (codec == FLV_VIDEO_AVCC || codec == FLV_VIDEO_HVCC)
    {
        return handle_video_config(codec, data);
    }
    if (codec == FLV_AUDIO_ASC || codec == FLV_AUDIO_OPUS_HEAD)
    {
        return handle_audio_config(codec, data);
    }
    if (codec == FLV_AUDIO_G711A || codec == FLV_AUDIO_G711U)
    {
        const auto result = initialize_g711_track(codec);
        if (result != 0)
        {
            return result;
        }
    }
    return publish_media(codec, data, pts, dts, flags);
}

int rtmp_publish_session::register_stream_if_ready()
{
    if (!shutdown_handler_ || !stream_->tracks().empty() || !expected_audio_.has_value() || !initial_video_track_ ||
        (*expected_audio_ && !initial_audio_track_))
    {
        return 0;
    }

    std::vector<media_track> tracks;
    tracks.push_back(*initial_video_track_);
    if (*expected_audio_)
    {
        tracks.push_back(*initial_audio_track_);
    }
    if (!stream_->set_tracks(std::move(tracks)))
    {
        return -1;
    }
    if (!stream_registry::instance().add(stream_))
    {
        spdlog::warn("rtmp publish duplicate stream {}", stream_->name());
        return -1;
    }
    initial_video_track_.reset();
    initial_audio_track_.reset();
    spdlog::info("rtmp publish tracks ready audio {}", *expected_audio_);
    return 0;
}

}    // namespace media_server
