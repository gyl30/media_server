#include <spdlog/spdlog.h>

#include "media/flv/flv_muxer.h"
#include "media/codec/codec_utils.h"

extern "C"
{
#include "flv-muxer.h"
#include "flv-proto.h"
#include "opus-head.h"
}

namespace media_server
{

flv_muxer::flv_muxer(packet_handler handler) : packet_handler_(std::move(handler)), muxer_(flv_muxer_create(&flv_muxer::on_packet, this)) {}

void flv_muxer::shutdown()
{
    packet_handler_ = {};
    tracks_.clear();
    if (muxer_ != nullptr)
    {
        flv_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
}

bool flv_muxer::on_track(const media_track& track)
{
    if (muxer_ == nullptr)
    {
        return false;
    }
    if ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) &&
        (track.clock_rate != 8'000 || track.channel_count != 1 || !track.codec_config.empty()))
    {
        return false;
    }
    if (track.codec == codec_id::opus &&
        (track.clock_rate != 48'000 || (track.channel_count != 1 && track.channel_count != 2) || !track.codec_config.empty()))
    {
        return false;
    }
    const auto existing = tracks_.find(track.id);
    if (existing != tracks_.end())
    {
        return true;
    }

    tracks_.emplace(track.id, track);
    return write_track_config(track, 0);
}

bool flv_muxer::write_track_config(const media_track& track, std::uint32_t timestamp)
{
    if (track.codec_config.empty() && track.codec != codec_id::opus)
    {
        return true;
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
            return false;
        }
        result = flv_muxer_opus(muxer_, head_data.data(), static_cast<std::size_t>(bytes), timestamp, timestamp);
    }
    else
    {
        return true;
    }

    if (result != 0)
    {
        spdlog::error("flv write track config codec {} result {}", to_string(track.codec), result);
        return false;
    }
    return true;
}

bool flv_muxer::on_frame(const media_frame& frame)
{
    if (muxer_ == nullptr)
    {
        return false;
    }
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || !frame.payload)
    {
        return true;
    }

    const auto pts = ns_to_flv_milliseconds(frame.pts_ns);
    const auto dts = ns_to_flv_milliseconds(frame.dts_ns);
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
    }

    if (result != 0)

    {
        spdlog::error("flv mux failed track {} result {}", frame.track, result);
        return false;
    }
    return true;
}

int flv_muxer::on_packet(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<flv_muxer*>(param);
    if (!self->packet_handler_ || data == nullptr)
    {
        return -1;
    }

    return self->packet_handler_(type, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes), timestamp);
}

}    // namespace media_server
