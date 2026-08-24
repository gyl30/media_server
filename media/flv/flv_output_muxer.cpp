#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/flv/flv_output_muxer.h"

extern "C"
{
#include "flv-muxer.h"
#include "flv-proto.h"
#include "opus-head.h"
}

namespace media_server
{

flv_output_muxer::flv_output_muxer(output_handler handler, output_video_config video)
    : handler_(std::move(handler)), video_config_(video), muxer_(flv_muxer_create(&flv_output_muxer::on_output, this))
{
    if (muxer_ != nullptr && video_config_.codec == output_video_codec::av1)
    {
        flv_muxer_set_enhanced_rtmp(muxer_, 1);
    }
}

flv_output_muxer::~flv_output_muxer()
{
    if (muxer_ != nullptr)
    {
        flv_muxer_destroy(muxer_);
    }
}

void flv_output_muxer::on_track(const media_track& track)
{
    if ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) &&
        (track.clock_rate != 8'000 || track.channel_count != 1 || !track.codec_config.empty()))
    {
        return;
    }
    if (track.codec == codec_id::opus &&
        (track.clock_rate != 48'000 || (track.channel_count != 1 && track.channel_count != 2) || !track.codec_config.empty()))
    {
        return;
    }
    const auto existing = tracks_.find(track.id);
    if (existing != tracks_.end() && existing->second.config_version == track.config_version)
    {
        return;
    }

    const bool reconfigured = existing != tracks_.end();
    tracks_.insert_or_assign(track.id, track);
    if (video_config_.codec == output_video_codec::av1 && track.kind == media_kind::video)
    {
        startup_video_transcoder(track);
    }

    if (reconfigured)
    {
        if (video_config_.codec == output_video_codec::av1 && track.kind == media_kind::audio)
        {
            if (track.codec == codec_id::opus)
            {
                video_config_pending_ = true;
            }
            return;
        }
        // flv_muxer 会缓存视频 sequence-header 状态，视频配置代际变化时统一重置。
        static_cast<void>(flv_muxer_reset(muxer_));
        video_config_pending_ = true;
        return;
    }

    prime_video_config(track, 0);
}

void flv_output_muxer::prime_video_config(const media_track& track, std::uint32_t timestamp)
{
    if (video_config_.codec == output_video_codec::av1 && track.kind == media_kind::video)
    {
        return;
    }
    if (track.codec_config.empty() && track.codec != codec_id::opus)
    {
        return;
    }

    int result = 0;
    if (track.codec == codec_id::h264)
    {
        result = flv_muxer_avc(muxer_, track.codec_config.data(), track.codec_config.size(), timestamp, timestamp);
    }
    else if (track.codec == codec_id::h265)
    {
        result = flv_muxer_hevc(muxer_, track.codec_config.data(), track.codec_config.size(), timestamp, timestamp);
    }
    else if (track.codec == codec_id::opus)
    {
        std::array<std::uint8_t, 29> head_data{};
        const opus_head_t head{
            .version = 1,
            .channels = static_cast<std::uint8_t>(track.channel_count),
            .pre_skip = 0,
            .input_sample_rate = 48'000,
            .output_gain = 0,
            .channel_mapping_family = 0,
            .stream_count = 0,
            .coupled_count = 0,
            .channel_mapping = {},
        };
        const auto bytes = opus_head_save(&head, head_data.data(), head_data.size());
        if (bytes <= 0)
        {
            spdlog::error("flv opus head create failed track {}", track.id);
            return;
        }
        result = flv_muxer_opus(muxer_, head_data.data(), static_cast<std::size_t>(bytes), timestamp, timestamp);
    }
    else
    {
        return;
    }

    if (result != 0)
    {
        spdlog::error("flv prime video config codec {} result {}", to_string(track.codec), result);
    }
}

void flv_output_muxer::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || !frame.payload)
    {
        return;
    }

    const auto pts = ns_to_flv_milliseconds(frame.pts_ns);
    const auto dts = ns_to_flv_milliseconds(frame.dts_ns);
    if (video_config_pending_)
    {
        for (const auto& [id, current] : tracks_)
        {
            static_cast<void>(id);
            prime_video_config(current, dts);
        }
        video_config_pending_ = false;
    }

    if (video_config_.codec == output_video_codec::av1 && iterator->second.kind == media_kind::video)
    {
        input_av1(frame);
        return;
    }

    int result = -1;

    switch (iterator->second.codec)

    {
        case codec_id::h264:
            result = flv_muxer_avc(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::h265:
            result = flv_muxer_hevc(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::aac:
            result = flv_muxer_aac(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::opus:
            result = flv_muxer_opus(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::g711a:
            result = flv_muxer_g711a(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::g711u:
            result = flv_muxer_g711u(muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::av1:
            break;
    }

    if (result != 0)

    {
        spdlog::error("flv mux failed track {} result {}", frame.track, result);
    }
}

void flv_output_muxer::startup_video_transcoder(const media_track& track)
{
    video_transcoder_.reset();
    video_track_id_ = 0;
    if (track.codec != codec_id::h264 && track.codec != codec_id::h265)
    {
        return;
    }
    auto transcoder = std::make_unique<video_transcoder>();
    if (!transcoder->startup(video_transcoder_config{
            .input_codec = track.codec,
            .output_codec = codec_id::av1,
            .input_codec_config = track.codec_config,
        }))
    {
        spdlog::error("flv av1 transcoder startup failed track {}", track.id);
        return;
    }
    video_track_id_ = track.id;
    video_transcoder_ = std::move(transcoder);
}

void flv_output_muxer::input_av1(const media_frame& frame)
{
    if (!video_transcoder_ || frame.track != video_track_id_)
    {
        return;
    }
    std::vector<media_frame> output;
    if (!video_transcoder_->transcode(frame, output))
    {
        spdlog::error("flv av1 transcode failed track {}", frame.track);
        return;
    }
    for (const auto& encoded : output)
    {
        if (!encoded.payload)
        {
            continue;
        }
        const auto result = flv_muxer_av1(muxer_,
                                          encoded.payload->data(),
                                          encoded.payload->size(),
                                          ns_to_flv_milliseconds(encoded.pts_ns),
                                          ns_to_flv_milliseconds(encoded.dts_ns),
                                          encoded.key_frame ? 1 : 0);
        if (result != 0)
        {
            spdlog::error("flv av1 mux failed track {} result {}", frame.track, result);
        }
    }
}

int flv_output_muxer::on_output(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<flv_output_muxer*>(param);
    if (!self->handler_ || data == nullptr)
    {
        return 0;
    }

    self->handler_(type, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes), timestamp);
    return 0;
}

}    // namespace media_server
