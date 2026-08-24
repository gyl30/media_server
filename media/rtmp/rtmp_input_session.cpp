#include <array>
#include <vector>
#include <utility>
#include <string_view>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/rtmp/rtmp_input_session.h"

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

rtmp_input_session::rtmp_input_session(boost::asio::any_io_executor executor,
                                       stream_registry& registry,
                                       std::string stream_name,
                                       std::chrono::milliseconds initial_tracks_timeout,
                                       shutdown_handler on_shutdown)
    : registry_(registry),
      initial_tracks_timer_(executor),
      initial_tracks_timeout_(initial_tracks_timeout),
      stream_name_(std::move(stream_name)),
      stream_(std::make_shared<media_stream>(stream_name_, executor)),
      on_shutdown_(std::move(on_shutdown))
{
}

rtmp_input_session::~rtmp_input_session()
{
    if (demuxer_ != nullptr)
    {
        flv_demuxer_destroy(demuxer_);
    }
}

bool rtmp_input_session::startup()
{
    demuxer_ = flv_demuxer_create(&rtmp_input_session::demux_callback, this);
    if (demuxer_ == nullptr)
    {
        return false;
    }

    initial_tracks_timer_.expires_after(initial_tracks_timeout_);
    const auto self = shared_from_this();
    initial_tracks_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || self->tracks_initialized_)
            {
                return;
            }
            spdlog::warn("rtmp input initial tracks timeout stream {}", self->stream_name_);
            self->on_shutdown_();
        });
    return true;
}

void rtmp_input_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    initial_tracks_timer_.cancel();
    if (stream_)
    {
        registry_.remove(*stream_);
        stream_->end();
        stream_.reset();
    }
    if (demuxer_ != nullptr)
    {
        flv_demuxer_destroy(demuxer_);
        demuxer_ = nullptr;
    }
}

int rtmp_input_session::on_video(const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    if (closed_ || demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(demuxer_, FLV_TYPE_VIDEO, data, bytes, timestamp);
}

int rtmp_input_session::on_audio(const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    if (closed_ || demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(demuxer_, FLV_TYPE_AUDIO, data, bytes, timestamp);
}

int rtmp_input_session::on_script(std::span<const std::uint8_t> data)
{
    if (closed_ || data.empty())
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
    if ((metadata_received_ && expected_audio_ != audio) || (initial_audio_track_ && !audio))
    {
        return -1;
    }

    expected_audio_ = audio;
    metadata_received_ = true;
    try_initialize_tracks();
    return 0;
}

int rtmp_input_session::demux_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (data == nullptr)
    {
        return -1;
    }
    return static_cast<rtmp_input_session*>(param)->on_flv_demux(
        codec, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes), pts, dts, flags);
}

int rtmp_input_session::on_flv_demux(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (closed_ || !stream_)
    {
        return -1;
    }

    if (codec == FLV_VIDEO_AVCC || codec == FLV_VIDEO_HVCC)
    {
        const auto video_codec = codec == FLV_VIDEO_AVCC ? codec_id::h264 : codec_id::h265;
        if (initial_video_track_ && initial_video_track_->codec != video_codec)
        {
            spdlog::warn("rtmp input video codec change {} {}", to_string(initial_video_track_->codec), to_string(video_codec));
            return -1;
        }

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
        if (!tracks_initialized_)
        {
            initial_video_track_ = std::move(track);
            try_initialize_tracks();
            return 0;
        }
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track video {}", to_string(video_codec));
        }
        return 0;
    }

    if (codec == FLV_AUDIO_ASC)
    {
        if (metadata_received_ && !expected_audio_)
        {
            return -1;
        }
        if (initial_audio_track_ && initial_audio_track_->codec != codec_id::aac)
        {
            spdlog::warn("rtmp input audio codec change {} aac", to_string(initial_audio_track_->codec));
            return -1;
        }
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
        if (!tracks_initialized_)
        {
            initial_audio_track_ = std::move(track);
            try_initialize_tracks();
            return 0;
        }
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track audio aac sample_rate {} channels {}", config->sample_rate, config->channel_count);
        }
        return 0;
    }

    if (codec == FLV_AUDIO_OPUS_HEAD)
    {
        if (metadata_received_ && !expected_audio_)
        {
            return -1;
        }
        if (initial_audio_track_ && initial_audio_track_->codec != codec_id::opus)
        {
            spdlog::warn("rtmp input audio codec change {} opus", to_string(initial_audio_track_->codec));
            return -1;
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
        if (!tracks_initialized_)
        {
            initial_audio_track_ = std::move(track);
            try_initialize_tracks();
            return 0;
        }
        static_cast<void>(stream_->update_track(std::move(track)));
        return 0;
    }

    if (codec == FLV_AUDIO_G711A || codec == FLV_AUDIO_G711U)
    {
        if (metadata_received_ && !expected_audio_)
        {
            return -1;
        }
        const auto audio_codec = codec == FLV_AUDIO_G711A ? codec_id::g711a : codec_id::g711u;
        if (!tracks_initialized_)
        {
            if (initial_audio_track_ && initial_audio_track_->codec != audio_codec)
            {
                spdlog::warn("rtmp input audio codec change {} {}", to_string(initial_audio_track_->codec), to_string(audio_codec));
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
            try_initialize_tracks();
            if (!tracks_initialized_)
            {
                return 0;
            }
        }
    }

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
    const auto& fixed = id == video_track_id ? initial_video_track_ : initial_audio_track_;
    if (fixed && fixed->codec != incoming_codec)
    {
        spdlog::warn("rtmp input raw codec change {} {}", to_string(fixed->codec), to_string(incoming_codec));
        return -1;
    }
    if (!tracks_initialized_)
    {
        return 0;
    }
    if (!fixed)
    {
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

void rtmp_input_session::try_initialize_tracks()
{
    if (tracks_initialized_ || !metadata_received_ || !initial_video_track_ || (expected_audio_ && !initial_audio_track_))
    {
        return;
    }

    if (std::chrono::steady_clock::now() >= initial_tracks_timer_.expiry())
    {
        on_shutdown_();
        return;
    }

    std::vector<media_track> tracks;
    tracks.push_back(*initial_video_track_);
    if (expected_audio_)
    {
        tracks.push_back(*initial_audio_track_);
    }
    tracks_initialized_ = stream_->set_tracks(std::move(tracks));
    if (!tracks_initialized_)
    {
        return;
    }
    if (!registry_.add(stream_))
    {
        spdlog::warn("rtmp publish duplicate stream {}", stream_name_);
        on_shutdown_();
        return;
    }
    initial_tracks_timer_.cancel();
    spdlog::info("rtmp input tracks ready audio {}", expected_audio_);
}

}    // namespace media_server
