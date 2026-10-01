#include <array>
#include <utility>
#include <algorithm>
#include <string_view>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/webrtc/webrtc_packetizer.h"

extern "C"
{
#include "rtp-ext.h"
#include "rtp-packet.h"
#include "rtsp-muxer.h"
#include "rtp-payload.h"
#include "rtp-profile.h"
}

namespace media_server
{
namespace
{

constexpr std::uint32_t opus_sample_rate = 48'000U;
constexpr std::size_t rtcp_buffer_size = 4096;
constexpr std::size_t max_mid_size = 16;
constexpr std::string_view rtcp_name = "media_server";
bool rtcp_mux_payload_type_allowed(int payload_type) { return payload_type >= 0 && payload_type <= 127 && (payload_type < 64 || payload_type > 95); }

}    // namespace

webrtc_packetizer::webrtc_packetizer(packet_handler rtp_handler, packet_handler rtcp_handler)
    : rtp_handler_(std::move(rtp_handler)),
      rtcp_handler_(std::move(rtcp_handler)),
      muxer_(rtsp_muxer_create(&webrtc_packetizer::on_packet, this))
{
}

webrtc_packetizer::~webrtc_packetizer()
{
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
    }
}

bool webrtc_packetizer::startup(std::span<const media_track> tracks, const webrtc_packetizer_config& config)
{
    if (muxer_ == nullptr)
    {
        return false;
    }
    for (const auto& track : tracks)
    {
        const bool video = track.kind == media_kind::video;
        const auto payload_type = video ? config.video_payload_type : config.audio_payload_type;
        if (payload_type < 0 || track.codec != (video ? config.video_codec : config.audio_codec))
        {
            continue;
        }
        const auto& mid = video ? config.video_mid : config.audio_mid;
        const auto extension_id = video ? config.video_mid_extension_id : config.audio_mid_extension_id;
        if (!rtcp_mux_payload_type_allowed(payload_type) || mid.empty() || mid.size() > max_mid_size || extension_id <= 0 || extension_id > 255)
        {
            return false;
        }

        std::vector<std::uint8_t> codec_config;
        const char* encoding{};
        int rtp_codec{};
        if (video)
        {
            const bool h264 = track.codec == codec_id::h264;
            codec_config = h264 ? h264_annex_b_to_avcc(track.codec_config) : h265_annex_b_to_hvcc(track.codec_config);
            if (codec_config.empty())
            {
                return false;
            }
            encoding = h264 ? "H264" : "H265";
            rtp_codec = h264 ? RTP_PAYLOAD_H264 : RTP_PAYLOAD_H265;
        }
        else
        {
            if (track.codec == codec_id::opus &&
                (track.clock_rate != opus_sample_rate || (track.channel_count != 1 && track.channel_count != 2) || !track.codec_config.empty()))
            {
                return false;
            }
            if ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) &&
                (track.clock_rate != 8'000 || track.channel_count != 1 || !track.codec_config.empty()))
            {
                return false;
            }
            const bool g711a = track.codec == codec_id::g711a;
            const bool g711u = track.codec == codec_id::g711u;
            encoding = g711a ? "PCMA" : (g711u ? "PCMU" : "opus");
            rtp_codec = g711a ? RTP_PAYLOAD_PCMA : (g711u ? RTP_PAYLOAD_PCMU : RTP_PAYLOAD_OPUS);
        }

        const auto payload_index = rtsp_muxer_add_payload(muxer_,
                                                         "RTP/AVP",
                                                         static_cast<int>(track.clock_rate),
                                                         payload_type,
                                                         encoding,
                                                         0,
                                                         0,
                                                         0,
                                                         codec_config.data(),
                                                         static_cast<int>(codec_config.size()));
        if (payload_index < 0)
        {
            spdlog::error("webrtc add payload failed codec {}", to_string(track.codec));
            return false;
        }
        const auto rtcp_result = rtsp_muxer_set_info(muxer_, payload_index, config.rtcp_cname.c_str(), rtcp_name.data());
        if (rtcp_result < 0)
        {
            spdlog::error("webrtc rtcp sender info failed payload {} result {}", payload_index, rtcp_result);
            return false;
        }
        const auto media_index =
            rtsp_muxer_add_media(muxer_, payload_index, rtp_codec, codec_config.data(), static_cast<int>(codec_config.size()));
        if (media_index < 0)
        {
            spdlog::error("webrtc add media failed codec {}", to_string(track.codec));
            return false;
        }
        track_states_.emplace(track.id,
                              track_state{
                                  .codec = track.codec,
                                  .media_index = media_index,
                                  .payload_index = payload_index,
                                  .mid = mid,
                                  .mid_extension_id = extension_id,
                              });
        spdlog::debug("webrtc packetizer track ready id {} codec {} pt {}", track.id, to_string(track.codec), payload_type);
    }
    return true;
}

bool webrtc_packetizer::on_frame(const media_frame& frame)
{
    const auto iterator = track_states_.find(frame.track);
    if (iterator == track_states_.end())
    {
        return true;
    }

    auto& state = iterator->second;
    if (state.codec == codec_id::h264 || state.codec == codec_id::h265)
    {
        return input_video(state, frame);
    }
    return input_audio(state, frame);
}

int webrtc_packetizer::on_packet(void* param, int payload_index, const void* data, int bytes, std::uint32_t, int)
{
    auto* self = static_cast<webrtc_packetizer*>(param);
    if (bytes <= 0 || data == nullptr)
    {
        return 0;
    }

    const auto state = std::find_if(self->track_states_.begin(),
                                    self->track_states_.end(),
                                    [payload_index](const auto& entry) { return entry.second.payload_index == payload_index; });
    if (state == self->track_states_.end())
    {
        return -1;
    }

    rtp_packet_t parsed{};
    if (rtp_packet_deserialize(&parsed, data, bytes) != 0)
    {
        return -1;
    }

    const auto& mid = state->second.mid;
    const auto extension_id = state->second.mid_extension_id;
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

    return self->rtp_handler_(packet);
}

bool webrtc_packetizer::emit_rtcp(int payload_index)
{
    std::array<std::uint8_t, rtcp_buffer_size> buffer{};
    const auto bytes = rtsp_muxer_rtcp(muxer_, payload_index, buffer.data(), static_cast<int>(buffer.size()));
    if (bytes < 0)
    {
        spdlog::error("webrtc rtcp report failed payload {} result {}", payload_index, bytes);
        return false;
    }
    if (bytes == 0)
    {
        return true;
    }
    if (static_cast<std::size_t>(bytes) > buffer.size())
    {
        spdlog::error("webrtc rtcp report too large payload {} bytes {}", payload_index, bytes);
        return false;
    }

    spdlog::trace("webrtc rtcp report generated payload {} size {}", payload_index, bytes);
    return rtcp_handler_(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(bytes))) == 0;
}

bool webrtc_packetizer::input_video(track_state& state, const media_frame& frame)
{
    const auto result = rtsp_muxer_input(muxer_,
                                         state.media_index,
                                         ns_to_milliseconds(frame.pts_ns),
                                         ns_to_milliseconds(frame.dts_ns),
                                         frame.payload->data(),
                                         static_cast<int>(frame.payload->size()),
                                         frame.key_frame ? 1 : 0);
    if (result < 0)
    {
        spdlog::error("webrtc video rtp packetize failed codec {} result {}", to_string(state.codec), result);
        return false;
    }
    return emit_rtcp(state.payload_index);
}

bool webrtc_packetizer::input_audio(track_state& state, const media_frame& frame)
{
    constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
    if ((frame.pts_ns % nanoseconds_per_millisecond) != 0 || (frame.dts_ns % nanoseconds_per_millisecond) != 0)
    {
        spdlog::error(
            "webrtc audio passthrough timestamp precision unsupported track {} pts_ns {} dts_ns {}", frame.track, frame.pts_ns, frame.dts_ns);
        return false;
    }

    const auto packet_size = rtp_packet_getsize();
    const auto extension_bytes = 4U + (((state.mid_extension_id > 14 ? 2U : 1U) + state.mid.size() + 3U) & ~std::size_t{3U});
    const auto payload_capacity = packet_size - RTP_FIXED_HEADER - static_cast<int>(extension_bytes);
    if (frame.payload->size() > static_cast<std::size_t>(payload_capacity))
    {
        spdlog::error(
            "webrtc audio passthrough packet too large track {} bytes {} capacity {}", frame.track, frame.payload->size(), payload_capacity);
        return false;
    }

    const auto result = rtsp_muxer_input(muxer_,
                                         state.media_index,
                                         ns_to_milliseconds(frame.pts_ns),
                                         ns_to_milliseconds(frame.dts_ns),
                                         frame.payload->data(),
                                         static_cast<int>(frame.payload->size()),
                                         0);
    if (result < 0)
    {
        spdlog::error("webrtc audio rtp packetize failed codec {} result {}", to_string(state.codec), result);
        return false;
    }
    return emit_rtcp(state.payload_index);
}

}    // namespace media_server
