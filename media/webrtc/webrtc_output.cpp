#include "media/webrtc/webrtc_output.h"

#include "media/codec/codec_utils.h"
#include "media/core/log.h"

extern "C"
{
#include "rtsp-muxer.h"
#include "rtp-profile.h"
}

#include <random>

namespace media_server
{

namespace
{
std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}
}    // namespace

webrtc_output::webrtc_output(rtp_handler handler)
    : handler_(std::move(handler)), muxer_(rtsp_muxer_create(&webrtc_output::on_packet, this))
{
}

webrtc_output::~webrtc_output()
{
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
    }
}

void webrtc_output::on_track(const media_track& track)
{
    if (track.codec != codec_id::h264 || muxer_ == nullptr)
    {
        return;
    }

    const auto avcc = h264_annex_b_to_avcc(track.codec_config);
    const auto payload_index = rtsp_muxer_add_payload(
        muxer_,
        "RTP/AVP",
        90'000,
        96,
        "H264",
        0,
        random_u32(),
        0,
        avcc.data(),
        static_cast<int>(avcc.size()));
    if (payload_index < 0)
    {
        log_line("webrtc", "add h264 payload failed");
        return;
    }

    const auto media_id = rtsp_muxer_add_media(
        muxer_, payload_index, RTP_PAYLOAD_H264, avcc.data(), static_cast<int>(avcc.size()));
    if (media_id < 0)
    {
        log_line("webrtc", "add h264 media failed");
        return;
    }

    tracks_.insert_or_assign(track.id, track_state{.track = track, .media_id = media_id});
}

void webrtc_output::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || iterator->second.media_id < 0 || !frame.payload)
    {
        return;
    }

    const auto result = rtsp_muxer_input(
        muxer_,
        iterator->second.media_id,
        ns_to_milliseconds(frame.pts_ns),
        ns_to_milliseconds(frame.dts_ns),
        frame.payload->data(),
        static_cast<int>(frame.payload->size()),
        frame.key_frame ? 1 : 0);
    if (result < 0)
    {
        log_line("webrtc", "rtp packetize failed", result);
    }
}

void webrtc_output::on_end()
{
}

std::size_t webrtc_output::packet_count() const noexcept
{
    return packet_count_;
}

int webrtc_output::on_packet(
    void* param,
    int,
    const void* data,
    int bytes,
    std::uint32_t,
    int)
    {

    auto* self = static_cast<webrtc_output*>(param);
    if (bytes <= 0 || data == nullptr)
    {
        return 0;
    }
    ++self->packet_count_;
    if (self->handler_)
    {
        self->handler_(std::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes)));
    }
    return 0;
}

}    // namespace media_server
