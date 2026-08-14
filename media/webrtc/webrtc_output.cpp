#include "media/webrtc/webrtc_output.h"

#include "media/codec/codec_utils.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "rtp-ext.h"
#include "rtp-packet.h"
#include "rtp-profile.h"
#include "rtsp-muxer.h"
}

#include <algorithm>
#include <array>
#include <random>
#include <string_view>
#include <utility>

namespace media_server
{
namespace
{

constexpr std::int64_t nanoseconds_per_second = 1'000'000'000LL;
constexpr std::int64_t opus_sample_rate = 48'000LL;
constexpr std::size_t rtcp_buffer_size = 4096;
constexpr std::size_t max_mid_size = 16;
constexpr std::string_view rtcp_name = "media_server";

bool rtcp_mux_payload_type_allowed(int payload_type)
{
    return payload_type >= 0 && payload_type <= 127 && (payload_type < 64 || payload_type > 95);
}

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

std::int64_t opus_samples_to_nanoseconds(std::uint32_t sample_count)
{
    return static_cast<std::int64_t>(sample_count) * nanoseconds_per_second / opus_sample_rate;
}

}    // namespace

webrtc_output::webrtc_output(webrtc_output_config config, packet_handler rtp_handler, packet_handler rtcp_handler)
    : config_(std::move(config)),
      rtp_handler_(std::move(rtp_handler)),
      rtcp_handler_(std::move(rtcp_handler)),
      muxer_(rtsp_muxer_create(&webrtc_output::on_packet, this))
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

    bool negotiated = false;
    bool added = false;
    if (track.kind == media_kind::video && config_.video_payload_type >= 0 && track.codec == config_.video_codec)
    {
        negotiated = true;
        if (track.codec == codec_id::h264)
        {
            added = add_h264_track(track);
        }
        else if (track.codec == codec_id::h265)
        {
            added = add_h265_track(track);
        }
    }
    else if (track.kind == media_kind::audio && config_.opus_payload_type >= 0 && track.codec == codec_id::aac)
    {
        negotiated = true;
        added = add_aac_track(track);
    }

    if (negotiated && !added)
    {
        tracks_.clear();
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
}

bool webrtc_output::valid() const noexcept { return muxer_ != nullptr; }

void webrtc_output::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || iterator->second.media_id < 0 || !frame.payload)
    {
        return;
    }

    auto& state = iterator->second;
    if (state.codec == codec_id::h264 || state.codec == codec_id::h265)
    {
        input_video(state, frame);
    }
    else if (state.codec == codec_id::aac)
    {
        input_aac(state, frame);
    }
}

int webrtc_output::on_packet(void* param, int pid, const void* data, int bytes, std::uint32_t, int)
{
    auto* self = static_cast<webrtc_output*>(param);
    if (bytes <= 0 || data == nullptr)
    {
        return 0;
    }

    const auto state = std::find_if(self->tracks_.begin(), self->tracks_.end(), [pid](const auto& entry) { return entry.second.payload_id == pid; });
    if (state == self->tracks_.end())
    {
        return -1;
    }

    rtp_packet_t parsed{};
    if (rtp_packet_deserialize(&parsed, data, bytes) != 0)
    {
        return -1;
    }

    const bool video = state->second.codec == codec_id::h264 || state->second.codec == codec_id::h265;
    const auto& mid = video ? self->config_.video_mid : self->config_.audio_mid;
    const auto extension_id = video ? self->config_.video_mid_extension_id : self->config_.audio_mid_extension_id;
    std::vector<std::uint8_t> extension;
    std::uint16_t extension_profile = RTP_HDREXT_PROFILE_TWO_BYTE;
    const bool two_byte_extension = extension_id > 14;
    if (!two_byte_extension)
    {
        extension_profile = RTP_HDREXT_PROFILE_ONE_BYTE;
        extension.push_back(static_cast<std::uint8_t>((extension_id << 4) | (static_cast<int>(mid.size()) - 1)));
    }
    else
    {
        extension.push_back(static_cast<std::uint8_t>(extension_id));
        extension.push_back(static_cast<std::uint8_t>(mid.size()));
    }
    extension.insert(extension.end(), mid.begin(), mid.end());
    while ((extension.size() % 4U) != 0U)
    {
        extension.push_back(0);
    }

    parsed.rtp.x = 1;
    parsed.extension = extension.data();
    parsed.extlen = static_cast<std::uint16_t>(extension.size());
    parsed.extprofile = extension_profile;

    std::vector<std::uint8_t> packet(static_cast<std::size_t>(bytes) + 4U + extension.size());
    const auto packet_bytes = rtp_packet_serialize(&parsed, packet.data(), static_cast<int>(packet.size()));
    if (packet_bytes <= 0)
    {
        return -1;
    }
    packet.resize(static_cast<std::size_t>(packet_bytes));

    const auto sequence = parsed.rtp.seq;
    spdlog::trace("webrtc rtp packet pt {} seq {} timestamp {} ssrc {} marker {} mid {} size {}",
                  static_cast<unsigned>(parsed.rtp.pt),
                  sequence,
                  parsed.rtp.timestamp,
                  parsed.rtp.ssrc,
                  parsed.rtp.m != 0,
                  mid,
                  packet.size());

    if (self->rtp_handler_)
    {
        self->rtp_handler_(packet);
    }
    return 0;
}

bool webrtc_output::add_h264_track(const media_track& track)
{
    if (!rtcp_mux_payload_type_allowed(config_.video_payload_type) || config_.video_mid.empty() || config_.video_mid.size() > max_mid_size ||
        config_.video_mid_extension_id <= 0 || config_.video_mid_extension_id > 255)
    {
        return false;
    }

    const auto avcc = h264_annex_b_to_avcc(track.codec_config);
    if (avcc.empty())
    {
        return false;
    }
    const auto payload_index = rtsp_muxer_add_payload(
        muxer_, "RTP/AVP", 90'000, config_.video_payload_type, "H264", 0, random_u32(), 0, avcc.data(), static_cast<int>(avcc.size()));
    if (payload_index < 0)
    {
        spdlog::error("webrtc add h264 payload failed");
        return false;
    }
    if (!configure_rtcp(payload_index))
    {
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(muxer_, payload_index, RTP_PAYLOAD_H264, avcc.data(), static_cast<int>(avcc.size()));
    if (media_id < 0)
    {
        spdlog::error("webrtc add h264 media failed");
        return false;
    }

    tracks_.insert_or_assign(track.id,
                             track_state{
                                 .codec = track.codec,
                                 .transcoder = {},
                                 .audio_pts_ns = 0,
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = true,
                                 .audio_pts_started = false,
                             });
    spdlog::debug("webrtc h264 output track ready id {} pt {}", track.id, config_.video_payload_type);
    return true;
}

bool webrtc_output::add_h265_track(const media_track& track)
{
    if (!rtcp_mux_payload_type_allowed(config_.video_payload_type) || config_.video_mid.empty() || config_.video_mid.size() > max_mid_size ||
        config_.video_mid_extension_id <= 0 || config_.video_mid_extension_id > 255)
    {
        return false;
    }

    const auto hvcc = h265_annex_b_to_hvcc(track.codec_config);
    if (hvcc.empty())
    {
        return false;
    }
    const auto payload_index = rtsp_muxer_add_payload(
        muxer_, "RTP/AVP", 90'000, config_.video_payload_type, "H265", 0, random_u32(), 0, hvcc.data(), static_cast<int>(hvcc.size()));
    if (payload_index < 0)
    {
        spdlog::error("webrtc add h265 payload failed");
        return false;
    }
    if (!configure_rtcp(payload_index))
    {
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(muxer_, payload_index, RTP_PAYLOAD_H265, hvcc.data(), static_cast<int>(hvcc.size()));
    if (media_id < 0)
    {
        spdlog::error("webrtc add h265 media failed");
        return false;
    }

    tracks_.insert_or_assign(track.id,
                             track_state{
                                 .codec = track.codec,
                                 .transcoder = {},
                                 .audio_pts_ns = 0,
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = true,
                                 .audio_pts_started = false,
                             });
    spdlog::debug("webrtc h265 output track ready id {} pt {}", track.id, config_.video_payload_type);
    return true;
}

bool webrtc_output::add_aac_track(const media_track& track)
{
    if (!rtcp_mux_payload_type_allowed(config_.opus_payload_type) || config_.audio_mid.empty() || config_.audio_mid.size() > max_mid_size ||
        config_.audio_mid_extension_id <= 0 || config_.audio_mid_extension_id > 255)
    {
        return false;
    }

    if (config_.opus_channel_count != 1 && config_.opus_channel_count != 2)
    {
        spdlog::error("webrtc invalid opus channel count {}", config_.opus_channel_count);
        return false;
    }

    auto transcoder = std::make_unique<aac_opus_transcoder>();
    const auto bitrate = config_.opus_bitrate > 0 ? config_.opus_bitrate : 64'000 * config_.opus_channel_count;
    if (!transcoder->startup(track.codec_config, config_.opus_channel_count, bitrate, config_.opus_max_playback_rate))
    {
        spdlog::error("webrtc aac opus transcoder startup failed track {}", track.id);
        return false;
    }

    const auto payload_index = rtsp_muxer_add_payload(muxer_, "RTP/AVP", 48'000, config_.opus_payload_type, "opus", 0, random_u32(), 0, nullptr, 0);
    if (payload_index < 0)
    {
        spdlog::error("webrtc add opus payload failed");
        return false;
    }
    if (!configure_rtcp(payload_index))
    {
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(muxer_, payload_index, RTP_PAYLOAD_OPUS, nullptr, 0);
    if (media_id < 0)
    {
        spdlog::error("webrtc add opus media failed");
        return false;
    }

    tracks_.insert_or_assign(track.id,
                             track_state{
                                 .codec = track.codec,
                                 .transcoder = std::move(transcoder),
                                 .audio_pts_ns = 0,
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = false,
                                 .audio_pts_started = false,
                             });
    spdlog::debug("webrtc opus output track ready id {} pt {} channels {}", track.id, config_.opus_payload_type, config_.opus_channel_count);
    return true;
}

bool webrtc_output::configure_rtcp(int payload_id)
{
    if (!rtcp_handler_)
    {
        return true;
    }
    if (config_.rtcp_cname.empty())
    {
        spdlog::error("webrtc rtcp cname missing payload {}", payload_id);
        return false;
    }

    const auto result = rtsp_muxer_set_info(muxer_, payload_id, config_.rtcp_cname.c_str(), rtcp_name.data());
    if (result < 0)
    {
        spdlog::error("webrtc rtcp sender info failed payload {} result {}", payload_id, result);
        return false;
    }
    return true;
}

void webrtc_output::emit_rtcp(int payload_id)
{
    if (!rtcp_handler_ || muxer_ == nullptr)
    {
        return;
    }

    std::array<std::uint8_t, rtcp_buffer_size> buffer{};
    const auto bytes = rtsp_muxer_rtcp(muxer_, payload_id, buffer.data(), static_cast<int>(buffer.size()));
    if (bytes < 0)
    {
        spdlog::error("webrtc rtcp report failed payload {} result {}", payload_id, bytes);
        return;
    }
    if (bytes == 0)
    {
        return;
    }
    if (static_cast<std::size_t>(bytes) > buffer.size())
    {
        spdlog::error("webrtc rtcp report too large payload {} bytes {}", payload_id, bytes);
        return;
    }

    spdlog::trace("webrtc rtcp report generated payload {} size {}", payload_id, bytes);
    rtcp_handler_(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(bytes)));
}

void webrtc_output::input_video(track_state& state, const media_frame& frame)
{
    if (state.waiting_key_frame)
    {
        if (!frame.key_frame)
        {
            return;
        }
        state.waiting_key_frame = false;
    }

    const auto result = rtsp_muxer_input(muxer_,
                                         state.media_id,
                                         ns_to_milliseconds(frame.pts_ns),
                                         ns_to_milliseconds(frame.dts_ns),
                                         frame.payload->data(),
                                         static_cast<int>(frame.payload->size()),
                                         frame.key_frame ? 1 : 0);
    if (result < 0)
    {
        spdlog::error("webrtc video rtp packetize failed codec {} result {}", to_string(state.codec), result);
        return;
    }
    emit_rtcp(state.payload_id);
}

void webrtc_output::input_aac(track_state& state, const media_frame& frame)
{
    if (!state.transcoder)
    {
        return;
    }
    if (!state.audio_pts_started)
    {
        state.audio_pts_ns = frame.pts_ns;
        state.audio_pts_started = true;
    }

    std::vector<opus_audio_packet> packets;
    if (!state.transcoder->transcode(*frame.payload, packets))
    {
        spdlog::error("webrtc aac opus transcode failed track {}", frame.track);
        return;
    }

    bool sent = false;
    for (const auto& packet : packets)
    {
        const auto pts_ms = ns_to_milliseconds(state.audio_pts_ns);
        const auto result =
            rtsp_muxer_input(muxer_, state.media_id, pts_ms, pts_ms, packet.payload.data(), static_cast<int>(packet.payload.size()), 0);
        if (result < 0)
        {
            spdlog::error("webrtc opus rtp packetize failed result {}", result);
            return;
        }
        state.audio_pts_ns += opus_samples_to_nanoseconds(packet.sample_count);
        sent = true;
    }

    if (sent)
    {
        emit_rtcp(state.payload_id);
    }
}

}    // namespace media_server
