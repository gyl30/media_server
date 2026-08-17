#include "media/webrtc/webrtc_output.h"

#include "media/codec/codec_utils.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "aom-av1.h"
#include "rtp-ext.h"
#include "rtp-packet.h"
#include "rtp-payload.h"
#include "rtp-profile.h"
#include "rtsp-muxer.h"
}

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace media_server
{
namespace
{

constexpr std::uint32_t opus_sample_rate = 48'000U;
constexpr std::size_t rtcp_buffer_size = 4096;
constexpr std::size_t max_mid_size = 16;
constexpr std::string_view rtcp_name = "media_server";
constexpr av1_encoding_parameters whep_av1_parameters{
    .profile = 0,
    .level_idx = 8,
    .tier = 0,
};

bool rtcp_mux_payload_type_allowed(int payload_type)
{
    return payload_type >= 0 && payload_type <= 127 && (payload_type < 64 || payload_type > 95);
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
    else if (track.kind == media_kind::video && config_.video_payload_type >= 0 && config_.video_codec == codec_id::av1 &&
             (track.codec == codec_id::h264 || track.codec == codec_id::h265))
    {
        negotiated = true;
        added = add_av1_track(track);
    }
    else if (track.kind == media_kind::audio && config_.audio_payload_type >= 0 && track.codec == config_.audio_codec &&
             (track.codec == codec_id::aac || track.codec == codec_id::opus || track.codec == codec_id::g711a || track.codec == codec_id::g711u))
    {
        negotiated = true;
        added = add_audio_track(track);
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
    if (state.codec == codec_id::h264 || state.codec == codec_id::h265 || state.codec == codec_id::av1)
    {
        input_video(state, frame);
    }
    else if (state.codec == codec_id::aac || state.codec == codec_id::opus || state.codec == codec_id::g711a || state.codec == codec_id::g711u)
    {
        input_audio(state, frame);
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

    const bool video = state->second.codec == codec_id::h264 || state->second.codec == codec_id::h265 || state->second.codec == codec_id::av1;
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
        muxer_, "RTP/AVP", 90'000, config_.video_payload_type, "H264", 0, 0, 0, avcc.data(), static_cast<int>(avcc.size()));
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
                                 .video_transcoder_ = {},
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = true,
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
        muxer_, "RTP/AVP", 90'000, config_.video_payload_type, "H265", 0, 0, 0, hvcc.data(), static_cast<int>(hvcc.size()));
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
                                 .video_transcoder_ = {},
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = true,
                             });
    spdlog::debug("webrtc h265 output track ready id {} pt {}", track.id, config_.video_payload_type);
    return true;
}

bool webrtc_output::add_av1_track(const media_track& track)
{
    if (!rtcp_mux_payload_type_allowed(config_.video_payload_type) || config_.video_mid.empty() || config_.video_mid.size() > max_mid_size ||
        config_.video_mid_extension_id <= 0 || config_.video_mid_extension_id > 255)
    {
        return false;
    }

    auto transcoder = std::make_unique<video_transcoder>();
    if (!transcoder->startup(video_transcoder_config{
            .input_codec = track.codec,
            .output_codec = codec_id::av1,
            .input_codec_config = track.codec_config,
            .av1 = whep_av1_parameters,
        }))
    {
        spdlog::error("webrtc av1 video transcoder startup failed track {}", track.id);
        return false;
    }

    aom_av1_t av1{};
    av1.marker = 1;
    av1.version = 1;
    av1.seq_profile = whep_av1_parameters.profile;
    av1.seq_level_idx_0 = whep_av1_parameters.level_idx;
    av1.seq_tier_0 = whep_av1_parameters.tier;
    av1.chroma_subsampling_x = 1;
    av1.chroma_subsampling_y = 1;
    std::array<std::uint8_t, 4> configuration_record{};
    if (aom_av1_codec_configuration_record_save(&av1, configuration_record.data(), configuration_record.size()) !=
        static_cast<int>(configuration_record.size()))
    {
        return false;
    }

    const auto payload_index = rtsp_muxer_add_payload(muxer_,
                                                       "RTP/AVP",
                                                       90'000,
                                                       config_.video_payload_type,
                                                       "AV1",
                                                       0,
                                                       0,
                                                       0,
                                                       configuration_record.data(),
                                                       static_cast<int>(configuration_record.size()));
    if (payload_index < 0)
    {
        spdlog::error("webrtc add av1 payload failed");
        return false;
    }
    if (!configure_rtcp(payload_index))
    {
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(
        muxer_, payload_index, RTP_PAYLOAD_AV1, configuration_record.data(), static_cast<int>(configuration_record.size()));
    if (media_id < 0)
    {
        spdlog::error("webrtc add av1 media failed");
        return false;
    }

    tracks_.insert_or_assign(track.id,
                             track_state{
                                 .codec = codec_id::av1,
                                 .transcoder = {},
                                 .video_transcoder_ = std::move(transcoder),
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .waiting_key_frame = true,
                             });
    spdlog::debug("webrtc av1 output track ready id {} pt {}", track.id, config_.video_payload_type);
    return true;
}

bool webrtc_output::add_audio_track(const media_track& track)
{
    if (!rtcp_mux_payload_type_allowed(config_.audio_payload_type) || config_.audio_mid.empty() || config_.audio_mid.size() > max_mid_size ||
        config_.audio_mid_extension_id <= 0 || config_.audio_mid_extension_id > 255)
    {
        return false;
    }

    if ((track.codec == codec_id::aac || track.codec == codec_id::opus) &&
        ((config_.opus_channel_count != 1 && config_.opus_channel_count != 2) || config_.opus_max_playback_rate < 8'000 ||
         config_.opus_max_playback_rate > 48'000))
    {
        spdlog::error("webrtc invalid opus output config channels {} max_playback_rate {}",
                      config_.opus_channel_count,
                      config_.opus_max_playback_rate);
        return false;
    }

    std::unique_ptr<audio_transcoder> transcoder;
    if (track.codec == codec_id::aac)
    {
        transcoder = std::make_unique<audio_transcoder>();
        const auto bitrate = config_.opus_bitrate > 0 ? config_.opus_bitrate : 64'000 * config_.opus_channel_count;
        int cutoff = 20'000;
        if (config_.opus_max_playback_rate <= 8'000)
        {
            cutoff = 4'000;
        }
        else if (config_.opus_max_playback_rate <= 12'000)
        {
            cutoff = 6'000;
        }
        else if (config_.opus_max_playback_rate <= 16'000)
        {
            cutoff = 8'000;
        }
        else if (config_.opus_max_playback_rate <= 24'000)
        {
            cutoff = 12'000;
        }
        if (!transcoder->startup(audio_transcoder_config{
                .input = audio_transcoder_format{
                    .codec = track.codec,
                    .sample_rate = track.clock_rate,
                    .channel_count = track.channel_count,
                },
                .output = audio_transcoder_format{
                    .codec = codec_id::opus,
                    .sample_rate = opus_sample_rate,
                    .channel_count = static_cast<std::uint16_t>(config_.opus_channel_count),
                },
                .input_codec_config = track.codec_config,
                .output_bit_rate = bitrate,
                .output_cutoff = cutoff,
            }))
        {
            spdlog::error("webrtc audio transcoder startup failed track {}", track.id);
            return false;
        }
    }
    else if (track.codec == codec_id::opus &&
             (track.clock_rate != opus_sample_rate || (track.channel_count != 1 && track.channel_count != 2) ||
              config_.opus_channel_count != track.channel_count || config_.opus_max_playback_rate != 48'000 || !track.codec_config.empty()))
    {
        return false;
    }
    else if ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) &&
             (track.clock_rate != 8'000 || track.channel_count != 1 || !track.codec_config.empty()))
    {
        return false;
    }

    const bool g711a = track.codec == codec_id::g711a;
    const bool g711u = track.codec == codec_id::g711u;
    const auto clock_rate = g711a || g711u ? 8'000 : 48'000;
    const auto encoding = g711a ? "PCMA" : (g711u ? "PCMU" : "opus");
    const auto rtp_codec = g711a ? RTP_PAYLOAD_PCMA : (g711u ? RTP_PAYLOAD_PCMU : RTP_PAYLOAD_OPUS);

    const auto payload_index =
        rtsp_muxer_add_payload(muxer_, "RTP/AVP", clock_rate, config_.audio_payload_type, encoding, 0, 0, 0, nullptr, 0);
    if (payload_index < 0)
    {
        spdlog::error("webrtc add audio payload failed");
        return false;
    }
    if (!configure_rtcp(payload_index))
    {
        return false;
    }

    const auto media_id = rtsp_muxer_add_media(muxer_, payload_index, rtp_codec, nullptr, 0);
    if (media_id < 0)
    {
        spdlog::error("webrtc add audio media failed");
        return false;
    }

    tracks_.insert_or_assign(track.id,
                             track_state{
                                 .codec = track.codec,
                                 .transcoder = std::move(transcoder),
                                 .video_transcoder_ = {},
                                 .media_id = media_id,
                                 .payload_id = payload_index,
                                 .rtp_extension_bytes = 4U +
                                     (((config_.audio_mid_extension_id > 14 ? 2U : 1U) + config_.audio_mid.size() + 3U) & ~std::size_t{3U}),
                                 .waiting_key_frame = false,
                             });
    spdlog::debug("webrtc audio output track ready id {} codec {} pt {} clock {}",
                  track.id,
                  to_string(track.codec),
                  config_.audio_payload_type,
                  clock_rate);
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
    if (state.video_transcoder_)
    {
        std::vector<media_frame> output;
        if (!state.video_transcoder_->transcode(frame, output))
        {
            spdlog::error("webrtc av1 video transcode failed track {}", frame.track);
            return;
        }

        bool sent = false;
        for (const auto& encoded : output)
        {
            if (!encoded.payload || (state.waiting_key_frame && !encoded.key_frame))
            {
                continue;
            }
            const auto result = rtsp_muxer_input(muxer_,
                                                 state.media_id,
                                                 ns_to_milliseconds(encoded.pts_ns),
                                                 ns_to_milliseconds(encoded.dts_ns),
                                                 encoded.payload->data(),
                                                 static_cast<int>(encoded.payload->size()),
                                                 encoded.key_frame ? 1 : 0);
            if (result < 0)
            {
                spdlog::error("webrtc av1 rtp packetize failed result {}", result);
                return;
            }
            sent = true;
            if (encoded.key_frame)
            {
                state.waiting_key_frame = false;
            }
        }
        if (sent)
        {
            emit_rtcp(state.payload_id);
        }
        return;
    }

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

void webrtc_output::input_audio(track_state& state, const media_frame& frame)
{
    if (state.codec == codec_id::opus || state.codec == codec_id::g711a || state.codec == codec_id::g711u)
    {
        constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
        if ((frame.pts_ns % nanoseconds_per_millisecond) != 0 || (frame.dts_ns % nanoseconds_per_millisecond) != 0)
        {
            spdlog::error("webrtc audio passthrough timestamp precision unsupported track {} pts_ns {} dts_ns {}",
                          frame.track,
                          frame.pts_ns,
                          frame.dts_ns);
            return;
        }

        const auto packet_size = rtp_packet_getsize();
        const auto payload_capacity = packet_size - RTP_FIXED_HEADER - static_cast<int>(state.rtp_extension_bytes);
        if (frame.payload->size() > static_cast<std::size_t>(payload_capacity))
        {
            spdlog::error("webrtc audio passthrough packet too large track {} bytes {} capacity {}",
                          frame.track,
                          frame.payload->size(),
                          payload_capacity);
            return;
        }

        const auto result = rtsp_muxer_input(muxer_,
                                             state.media_id,
                                             ns_to_milliseconds(frame.pts_ns),
                                             ns_to_milliseconds(frame.dts_ns),
                                             frame.payload->data(),
                                             static_cast<int>(frame.payload->size()),
                                             0);
        if (result < 0)
        {
            spdlog::error("webrtc audio rtp packetize failed codec {} result {}", to_string(state.codec), result);
            return;
        }
        emit_rtcp(state.payload_id);
        return;
    }

    if (!state.transcoder)
    {
        return;
    }
    std::vector<media_frame> packets;
    if (!state.transcoder->transcode(frame, packets))
    {
        spdlog::error("webrtc audio transcode failed track {}", frame.track);
        return;
    }

    bool sent = false;
    for (const auto& packet : packets)
    {
        const auto pts_ms = ns_to_milliseconds(packet.pts_ns);
        const auto result =
            rtsp_muxer_input(muxer_, state.media_id, pts_ms, pts_ms, packet.payload->data(), static_cast<int>(packet.payload->size()), 0);
        if (result < 0)
        {
            spdlog::error("webrtc opus rtp packetize failed result {}", result);
            return;
        }
        sent = true;
    }

    if (sent)
    {
        emit_rtcp(state.payload_id);
    }
}

}    // namespace media_server
