#include "media/flv/flv_output_muxer.h"

#include "media/codec/codec_utils.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "flv-muxer.h"
#include "flv-proto.h"
}

#include <limits>

namespace media_server
{

flv_output_muxer::flv_output_muxer(output_handler handler)
    : handler_(std::move(handler)), muxer_(flv_muxer_create(&flv_output_muxer::on_output, this))
{
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
    tracks_.insert_or_assign(track.id, track);

    if (track.codec == codec_id::h264 && !track.codec_config.empty())

    {
        // 仅 SPS/PPS，不包含 VCL。flv_muxer 会据此输出 AVC sequence header。
        const auto result = flv_muxer_avc(
            muxer_,
            track.codec_config.data(),
            track.codec_config.size(),
            0,
            0);
        if (result != 0)
        {
            spdlog::error("flv prime h264 config failed result {}", result);
        }
    }
}

void flv_output_muxer::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || !frame.payload)
    {
        return;
    }

    const auto pts = ns_to_milliseconds(frame.pts_ns);
    const auto dts = ns_to_milliseconds(frame.dts_ns);
    int result = -1;

    switch (iterator->second.codec)

    {
    case codec_id::h264:
        result = flv_muxer_avc(
            muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
        break;
    case codec_id::aac:
        result = flv_muxer_aac(
            muxer_, frame.payload->data(), frame.payload->size(), pts, dts);
        break;
    }

    if (result != 0)

    {
        spdlog::error("flv mux failed track {} result {}", frame.track, result);
    }
}

int flv_output_muxer::on_output(
    void* param,
    int type,
    const void* data,
    std::size_t bytes,
    std::uint32_t timestamp)
    {

    auto* self = static_cast<flv_output_muxer*>(param);
    if (!self->handler_ || data == nullptr)
    {
        return 0;
    }

    self->handler_(
        type,
        std::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(data), bytes),
        timestamp);
    return 0;
}

}    // namespace media_server
