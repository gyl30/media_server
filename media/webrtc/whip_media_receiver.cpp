#include <bit>
#include <array>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/webrtc/whip_media_receiver.h"

extern "C"
{
#include "avpacket.h"
#include "rtcp-header.h"
#include "rtsp-demuxer.h"
}

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
constexpr std::uint32_t opus_sample_rate = 48'000;
constexpr int audio_cutoff = 20'000;

bool rtcp_mux_payload_type_allowed(int payload_type) { return payload_type >= 0 && payload_type <= 127 && (payload_type < 64 || payload_type > 95); }

std::uint32_t read_u32(std::span<const std::uint8_t> packet, std::size_t offset)
{
    return (static_cast<std::uint32_t>(packet[offset]) << 24U) | (static_cast<std::uint32_t>(packet[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(packet[offset + 2U]) << 8U) | static_cast<std::uint32_t>(packet[offset + 3U]);
}

}    // namespace

whip_media_receiver::whip_media_receiver(worker_context& worker, std::string stream_name, whip_media_receiver_config config)
    : worker_(worker), stream_name_(std::move(stream_name)), config_(config)
{
}

whip_media_receiver::~whip_media_receiver() = default;

bool whip_media_receiver::startup()
{
    if (closed_ || media_stream_ || stream_name_.empty() || (config_.video_codec != codec_id::h264 && config_.video_codec != codec_id::h265) ||
        !rtcp_mux_payload_type_allowed(config_.video_payload_type) ||
        (config_.audio_payload_type >= 0 &&
         (!rtcp_mux_payload_type_allowed(config_.audio_payload_type) || config_.audio_payload_type == config_.video_payload_type ||
          (config_.audio_channel_count != 1 && config_.audio_channel_count != 2))))
    {
        return false;
    }

    media_stream_ = std::make_shared<media_stream>(stream_name_, worker_);
    static_cast<void>(avpkt2bs_create(&bitstream_));

    video_demuxer_ = rtsp_demuxer_create(0, 500, &whip_media_receiver::packet_callback, this);
    const auto* video_encoding = config_.video_codec == codec_id::h264 ? "H264" : "H265";
    if (video_demuxer_ == nullptr ||
        rtsp_demuxer_add_payload(video_demuxer_, 90'000, config_.video_payload_type, video_encoding, nullptr) != 0)
    {
        shutdown();
        return false;
    }

    if (config_.audio_payload_type >= 0)
    {
        audio_demuxer_ = rtsp_demuxer_create(1, 500, &whip_media_receiver::packet_callback, this);
        if (audio_demuxer_ == nullptr ||
            rtsp_demuxer_add_payload(audio_demuxer_, static_cast<int>(opus_sample_rate), config_.audio_payload_type, "opus", nullptr) != 0)
        {
            shutdown();
            return false;
        }

        const auto bitrate = 64'000 * static_cast<int>(config_.audio_channel_count);
        if (!audio_transcoder_.startup(audio_transcoder_config{
                .input =
                    audio_transcoder_format{
                        .codec = codec_id::opus,
                        .sample_rate = opus_sample_rate,
                        .channel_count = config_.audio_channel_count,
                    },
                .output =
                    audio_transcoder_format{
                        .codec = codec_id::aac,
                        .sample_rate = opus_sample_rate,
                        .channel_count = config_.audio_channel_count,
                    },
                .input_codec_config = {},
                .output_bit_rate = bitrate,
                .output_cutoff = audio_cutoff,
            }))
        {
            shutdown();
            return false;
        }
        const auto codec_config = audio_transcoder_.output_codec_config();
        if (codec_config.empty())
        {
            shutdown();
            return false;
        }
        audio_track_ = media_track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = codec_id::aac,
            .clock_rate = opus_sample_rate,
            .channel_count = config_.audio_channel_count,
            .codec_config = std::vector<std::uint8_t>(codec_config.begin(), codec_config.end()),
        };
    }
    return true;
}

bool whip_media_receiver::input_rtp(std::span<const std::uint8_t> packet)
{
    if (closed_ || !media_stream_ || packet.size() < 12U || (packet[0] >> 6U) != 2U)
    {
        return false;
    }

    const auto payload_type = static_cast<int>(packet[1] & 0x7fU);
    const auto ssrc = read_u32(packet, 8);
    rtsp_demuxer_t* demuxer{};
    std::optional<std::uint32_t>* expected_ssrc{};
    if (payload_type == config_.video_payload_type)
    {
        demuxer = video_demuxer_;
        expected_ssrc = &video_ssrc_;
    }
    else if (config_.audio_payload_type >= 0 && payload_type == config_.audio_payload_type)
    {
        demuxer = audio_demuxer_;
        expected_ssrc = &audio_ssrc_;
    }
    else
    {
        return false;
    }

    if (demuxer == nullptr || expected_ssrc == nullptr || (*expected_ssrc && **expected_ssrc != ssrc))
    {
        return false;
    }
    if (!*expected_ssrc)
    {
        *expected_ssrc = ssrc;
    }
    return rtsp_demuxer_input(demuxer, packet.data(), static_cast<int>(packet.size())) >= 0;
}

bool whip_media_receiver::input_rtcp(std::span<const std::uint8_t> packet)
{
    if (closed_ || !media_stream_ || packet.size() < 8U || (packet[0] >> 6U) != 2U)
    {
        return false;
    }
    const auto packet_type = packet[1];
    if (packet_type < 192U || packet_type > 223U)
    {
        return false;
    }
    const auto packet_bytes = (static_cast<std::size_t>((static_cast<std::uint16_t>(packet[2]) << 8U) | packet[3]) + 1U) * 4U;
    if (packet_bytes > packet.size())
    {
        return false;
    }
    if (packet_type != RTCP_SR)
    {
        return true;
    }

    const auto ssrc = read_u32(packet, 4);
    rtsp_demuxer_t* demuxer{};
    if (video_ssrc_ && *video_ssrc_ == ssrc)
    {
        demuxer = video_demuxer_;
    }
    else if (audio_ssrc_ && *audio_ssrc_ == ssrc)
    {
        demuxer = audio_demuxer_;
    }
    if (demuxer == nullptr)
    {
        return true;
    }

    const auto result = rtsp_demuxer_input(demuxer, packet.data(), static_cast<int>(packet.size()));
    return result >= 0 && (result != RTCP_SR || apply_sender_report(demuxer));
}

void whip_media_receiver::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (media_stream_)
    {
        stream_registry::instance().remove(*media_stream_);
        media_stream_->end();
        media_stream_.reset();
    }
    if (video_demuxer_ != nullptr)
    {
        rtsp_demuxer_destroy(video_demuxer_);
        video_demuxer_ = nullptr;
    }
    if (audio_demuxer_ != nullptr)
    {
        rtsp_demuxer_destroy(audio_demuxer_);
        audio_demuxer_ = nullptr;
    }
    audio_transcoder_.shutdown();
    avpkt2bs_destroy(&bitstream_);
    video_track_.reset();
    audio_track_.reset();
    video_ssrc_.reset();
    audio_ssrc_.reset();
    published_ = false;
    rtcp_synchronized_ = false;
}

int whip_media_receiver::packet_callback(void* param, avpacket_t* packet)
{
    return static_cast<whip_media_receiver*>(param)->on_demuxed_packet(packet);
}

int whip_media_receiver::on_demuxed_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || closed_ || !media_stream_)
    {
        return -1;
    }

    const auto codecid = packet->stream->codecid;
    const auto expected_video = config_.video_codec == codec_id::h264 ? AVCODEC_VIDEO_H264 : AVCODEC_VIDEO_H265;
    const bool video = codecid == AVCODEC_VIDEO_H264 || codecid == AVCODEC_VIDEO_H265;
    if ((video && codecid != expected_video) || (!video && codecid != AVCODEC_AUDIO_OPUS))
    {
        spdlog::warn("whip input codec changed");
        return -1;
    }

    if (video && !update_video_track(*packet))
    {
        return -1;
    }

    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes < 0 || (bytes > 0 && bitstream_.ptr == nullptr))
    {
        return -1;
    }
    if (bytes == 0)
    {
        return 0;
    }

    const auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    const media_frame frame{
        .track = video ? video_track_id : audio_track_id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = video && (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = payload,
    };

    if (video)
    {
        if (!published_ && video_track_ && !publish_stream())
        {
            return -1;
        }
        if (published_)
        {
            media_stream_->publish(frame);
        }
        return 0;
    }

    std::vector<media_frame> output;
    if (!audio_transcoder_.transcode(frame, output))
    {
        return -1;
    }
    if (published_)
    {
        for (auto& encoded : output)
        {
            media_stream_->publish(std::move(encoded));
        }
    }
    return 0;
}

bool whip_media_receiver::update_video_track(const avpacket_t& packet)
{
    auto track = media_track_from_avstream_config(*packet.stream, video_track_id, audio_track_id);
    if (!track)
    {
        return true;
    }
    if (track->codec != config_.video_codec)
    {
        return false;
    }

    const bool changed = !video_track_ || video_track_->clock_rate != track->clock_rate || video_track_->channel_count != track->channel_count ||
                         video_track_->codec_config != track->codec_config;
    if (!changed)
    {
        return true;
    }
    if (published_ && !media_stream_->update_track(*track))
    {
        return false;
    }
    video_track_ = std::move(*track);
    avpkt2bs_destroy(&bitstream_);
    static_cast<void>(avpkt2bs_create(&bitstream_));
    return true;
}

bool whip_media_receiver::publish_stream()
{
    if (published_ || !video_track_)
    {
        return published_;
    }

    std::vector<media_track> tracks;
    tracks.push_back(*video_track_);
    if (audio_track_)
    {
        tracks.push_back(*audio_track_);
    }
    if (!media_stream_->set_tracks(std::move(tracks)) || !stream_registry::instance().add(media_stream_))
    {
        return false;
    }
    published_ = true;
    return true;
}

bool whip_media_receiver::apply_sender_report(rtsp_demuxer_t* demuxer)
{
    std::uint32_t ntp_msw{};
    std::uint32_t ntp_lsw{};
    std::uint32_t rtp_timestamp{};
    std::int64_t pts{};
    if (rtsp_demuxer_sender_report(demuxer, &ntp_msw, &ntp_lsw, &rtp_timestamp, &pts) != 0)
    {
        return false;
    }

    const auto ntp = (static_cast<std::uint64_t>(ntp_msw) << 32U) | ntp_lsw;
    if (!rtcp_synchronized_)
    {
        rtcp_sync_ntp_ = ntp;
        rtcp_sync_pts_ = pts;
        rtcp_synchronized_ = true;
    }
    else
    {
        constexpr std::int64_t ntp_fraction = std::int64_t{1} << 32U;
        const auto delta = std::bit_cast<std::int64_t>(ntp - rtcp_sync_ntp_);
        pts = rtcp_sync_pts_ + (delta / ntp_fraction) * 1'000 + (delta % ntp_fraction) * 1'000 / ntp_fraction;
    }
    return rtsp_demuxer_set_timestamp(demuxer, rtp_timestamp, pts) == 0;
}

}    // namespace media_server
