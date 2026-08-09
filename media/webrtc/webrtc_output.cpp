#include "media/webrtc/webrtc_output.h"

#include "media/codec/codec_utils.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "rtp-profile.h"
#include "rtsp-muxer.h"
}

#include <random>
#include <utility>

namespace media_server
{
namespace
{

constexpr std::int64_t nanoseconds_per_second = 1'000'000'000LL;
constexpr std::int64_t opus_sample_rate = 48'000LL;
constexpr std::int64_t rtp_timeline_origin_ms = 1;

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

std::int64_t opus_samples_to_nanoseconds(std::uint32_t sample_count)
{
    return static_cast<std::int64_t>(sample_count) * nanoseconds_per_second / opus_sample_rate;
}

std::int64_t rtp_milliseconds(std::int64_t nanoseconds)
{
    return ns_to_milliseconds(nanoseconds) + rtp_timeline_origin_ms;
}

}    // namespace

webrtc_output::webrtc_output(webrtc_output_config config, rtp_handler handler)
    : config_(config), handler_(std::move(handler)), muxer_(rtsp_muxer_create(&webrtc_output::on_packet, this))
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
    if (muxer_ == nullptr)
    {
        return;
    }

    if (track.codec == codec_id::h264)
    {
        static_cast<void>(add_h264_track(track));
    }
    else if (track.codec == codec_id::aac)
    {
        static_cast<void>(add_aac_track(track));
    }
}

void webrtc_output::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || iterator->second.media_id < 0 || !frame.payload)
    {
        return;
    }

    if (iterator->second.track.codec == codec_id::h264)
    {
        input_h264(iterator->first, frame);
    }
    else if (iterator->second.track.codec == codec_id::aac)
    {
        input_aac(iterator->first, frame);
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
    const auto packet = std::span<const std::uint8_t>(
        static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes));
    if (packet.size() >= 12U)
    {
        const auto sequence = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(packet[2]) << 8U) |
            static_cast<std::uint16_t>(packet[3]));
        const auto timestamp =
            (static_cast<std::uint32_t>(packet[4]) << 24U) |
            (static_cast<std::uint32_t>(packet[5]) << 16U) |
            (static_cast<std::uint32_t>(packet[6]) << 8U) |
            static_cast<std::uint32_t>(packet[7]);
        const auto ssrc =
            (static_cast<std::uint32_t>(packet[8]) << 24U) |
            (static_cast<std::uint32_t>(packet[9]) << 16U) |
            (static_cast<std::uint32_t>(packet[10]) << 8U) |
            static_cast<std::uint32_t>(packet[11]);
        spdlog::trace(
            "webrtc rtp packet pt {} seq {} timestamp {} ssrc {} marker {} size {}",
            packet[1] & 0x7fU,
            sequence,
            timestamp,
            ssrc,
            (packet[1] & 0x80U) != 0,
            packet.size());
    }

    if (self->handler_)
    {
        self->handler_(packet);
    }
    return 0;
}

bool webrtc_output::add_h264_track(const media_track& track)
{
    if (config_.h264_payload_type < 0 || config_.h264_payload_type > 127)
    {
        return false;
    }

    const auto avcc = h264_annex_b_to_avcc(track.codec_config);
    const auto payload_index = rtsp_muxer_add_payload(
        muxer_,
        "RTP/AVP",
        90'000,
        config_.h264_payload_type,
        "H264",
        0,
        random_u32(),
        0,
        avcc.data(),
        static_cast<int>(avcc.size()));
    if (payload_index < 0)
    {
        spdlog::error("webrtc add h264 payload failed");
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(
        muxer_, payload_index, RTP_PAYLOAD_H264, avcc.data(), static_cast<int>(avcc.size()));
    if (media_id < 0)
    {
        spdlog::error("webrtc add h264 media failed");
        return false;
    }

    tracks_.insert_or_assign(
        track.id,
        track_state{
            .track = track,
            .transcoder = {},
            .audio_pts_ns = 0,
            .media_id = media_id,
            .waiting_key_frame = true,
            .audio_pts_started = false,
        });
    spdlog::debug("webrtc h264 output track ready id {} pt {}", track.id, config_.h264_payload_type);
    return true;
}

bool webrtc_output::add_aac_track(const media_track& track)
{
    if (config_.opus_payload_type < 0 || config_.opus_payload_type > 127)
    {
        return false;
    }

    auto transcoder = std::make_unique<aac_opus_transcoder>();
    if (!transcoder->start(track.codec_config))
    {
        spdlog::error("webrtc aac opus transcoder start failed track {}", track.id);
        return false;
    }

    const auto payload_index = rtsp_muxer_add_payload(
        muxer_,
        "RTP/AVP",
        48'000,
        config_.opus_payload_type,
        "opus",
        0,
        random_u32(),
        0,
        nullptr,
        0);
    if (payload_index < 0)
    {
        spdlog::error("webrtc add opus payload failed");
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(muxer_, payload_index, RTP_PAYLOAD_OPUS, nullptr, 0);
    if (media_id < 0)
    {
        spdlog::error("webrtc add opus media failed");
        return false;
    }

    tracks_.insert_or_assign(
        track.id,
        track_state{
            .track = track,
            .transcoder = std::move(transcoder),
            .audio_pts_ns = 0,
            .media_id = media_id,
            .waiting_key_frame = false,
            .audio_pts_started = false,
        });
    spdlog::debug("webrtc opus output track ready id {} pt {}", track.id, config_.opus_payload_type);
    return true;
}

void webrtc_output::input_h264(track_id id, const media_frame& frame)
{
    auto iterator = tracks_.find(id);
    if (iterator == tracks_.end())
    {
        return;
    }

    auto& state = iterator->second;
    if (state.waiting_key_frame)
    {
        if (!frame.key_frame)
        {
            return;
        }
        state.waiting_key_frame = false;
    }

    const auto result = rtsp_muxer_input(
        muxer_,
        state.media_id,
        rtp_milliseconds(frame.pts_ns),
        rtp_milliseconds(frame.dts_ns),
        frame.payload->data(),
        static_cast<int>(frame.payload->size()),
        frame.key_frame ? 1 : 0);
    if (result < 0)
    {
        spdlog::error("webrtc h264 rtp packetize failed result {}", result);
    }
}

void webrtc_output::input_aac(track_id id, const media_frame& frame)
{
    auto iterator = tracks_.find(id);
    if (iterator == tracks_.end() || !iterator->second.transcoder)
    {
        return;
    }

    auto& state = iterator->second;
    if (!state.audio_pts_started)
    {
        state.audio_pts_ns = frame.pts_ns;
        state.audio_pts_started = true;
    }

    std::vector<opus_audio_packet> packets;
    if (!state.transcoder->transcode(*frame.payload, packets))
    {
        spdlog::error("webrtc aac opus transcode failed track {}", id);
        return;
    }

    for (const auto& packet : packets)
    {
        const auto pts_ms = rtp_milliseconds(state.audio_pts_ns);
        const auto result = rtsp_muxer_input(
            muxer_,
            state.media_id,
            pts_ms,
            pts_ms,
            packet.payload.data(),
            static_cast<int>(packet.payload.size()),
            0);
        if (result < 0)
        {
            spdlog::error("webrtc opus rtp packetize failed result {}", result);
            return;
        }
        state.audio_pts_ns += opus_samples_to_nanoseconds(packet.sample_count);
    }
}

}    // namespace media_server
