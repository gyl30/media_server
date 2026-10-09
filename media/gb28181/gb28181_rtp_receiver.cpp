#include <algorithm>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

extern "C"
{
#include "avpacket.h"
#include "mpeg-proto.h"
#include "mpeg-util.h"
#include "rtp-packet.h"
#include "rtsp-demuxer.h"
}

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

std::optional<codec_id> codec_from_ps(int codecid)
{
    switch (codecid)
    {
        case PSI_STREAM_H264:
            return codec_id::h264;
        case PSI_STREAM_H265:
            return codec_id::h265;
        case PSI_STREAM_AAC:
            return codec_id::aac;
        case PSI_STREAM_AUDIO_G711A:
            return codec_id::g711a;
        case PSI_STREAM_AUDIO_G711U:
            return codec_id::g711u;
        default:
            return std::nullopt;
    }
}

std::optional<codec_id> codec_from_avpacket(int codecid)
{
    switch (codecid)
    {
        case AVCODEC_VIDEO_H264:
            return codec_id::h264;
        case AVCODEC_VIDEO_H265:
            return codec_id::h265;
        case AVCODEC_AUDIO_AAC:
            return codec_id::aac;
        case AVCODEC_AUDIO_G711A:
            return codec_id::g711a;
        case AVCODEC_AUDIO_G711U:
            return codec_id::g711u;
        default:
            return std::nullopt;
    }
}

bool is_video(codec_id codec) { return codec == codec_id::h264 || codec == codec_id::h265; }

}    // namespace

gb28181_rtp_receiver::gb28181_rtp_receiver(worker_context& worker,
                                           std::string stream_name,
                                           std::uint8_t payload_type,
                                           std::uint32_t expected_ssrc,
                                           std::function<void()> media_handler)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      payload_type_(payload_type),
      expected_ssrc_(expected_ssrc),
      media_handler_(std::move(media_handler))
{
}

bool gb28181_rtp_receiver::startup()
{
    stream_ = std::make_shared<media_stream>(stream_name_, worker_);
    avpkt2bs_create(&bitstream_);
    demuxer_ = rtsp_demuxer_create(0, 500, &gb28181_rtp_receiver::packet_callback, this);
    if (demuxer_ == nullptr || rtsp_demuxer_add_payload(demuxer_, 90'000, payload_type_, "PS", nullptr) != 0 ||
        rtsp_demuxer_set_ps_notify(demuxer_, &gb28181_rtp_receiver::stream_callback, this) != 0 ||
        rtsp_demuxer_set_info(demuxer_, stream_name_.c_str(), "media_server") != 0)
    {
        shutdown();
        return false;
    }
    return true;
}

gb28181_rtp_receive_result gb28181_rtp_receiver::receive_rtp(std::span<const std::uint8_t> data)
{
    if (data.size() < 12)
    {
        return gb28181_rtp_receive_result::ignored;
    }

    rtp_packet_t packet{};
    // 空载荷包不是媒体：既不进入解复用，也不能让 UDP 会话锁定其来源端点。
    if (rtp_packet_deserialize(&packet, data.data(), static_cast<int>(data.size())) != 0 || packet.rtp.pt != payload_type_ ||
        packet.rtp.ssrc != expected_ssrc_ || packet.payloadlen <= 0)
    {
        return gb28181_rtp_receive_result::ignored;
    }

    const auto result = rtsp_demuxer_input(demuxer_, data.data(), static_cast<int>(data.size()));
    return result < 0 ? gb28181_rtp_receive_result::fatal : gb28181_rtp_receive_result::accepted;
}

bool gb28181_rtp_receiver::receive_rtcp(std::span<const std::uint8_t> data)
{
    return data.size() >= 4 && rtsp_demuxer_input(demuxer_, data.data(), static_cast<int>(data.size())) > 0;
}

int gb28181_rtp_receiver::generate_rtcp(std::span<std::uint8_t> buffer)
{
    return rtsp_demuxer_rtcp(demuxer_, buffer.data(), static_cast<int>(buffer.size()));
}

void gb28181_rtp_receiver::shutdown()
{
    if (stream_)
    {
        stream_registry::instance().remove(*stream_);
        stream_->end();
        stream_.reset();
    }
    if (demuxer_ != nullptr)
    {
        rtsp_demuxer_destroy(demuxer_);
        demuxer_ = nullptr;
    }
    avpkt2bs_destroy(&bitstream_);
}

const std::string& gb28181_rtp_receiver::stream_name() const noexcept { return stream_name_; }

int gb28181_rtp_receiver::packet_callback(void* param, avpacket_t* packet)
{
    return static_cast<gb28181_rtp_receiver*>(param)->on_demuxed_packet(packet);
}

void gb28181_rtp_receiver::stream_callback(void* param, int, int codecid, const void*, int, int finish)
{
    static_cast<gb28181_rtp_receiver*>(param)->on_stream(codecid, finish != 0);
}

void gb28181_rtp_receiver::on_stream(int codecid, bool finish)
{
    const auto codec = codec_from_ps(codecid);
    if (!codec && mpeg_stream_type_audio(codecid) != 0)
    {
        // 不支持的音频按无音频处理，视频照常输出。
        pending_topology_.unsupported_audio = true;
    }
    else if (!codec)
    {
        pending_topology_.invalid = true;
    }
    else if (is_video(*codec))
    {
        if (pending_topology_.video)
        {
            pending_topology_.invalid = true;
        }
        else
        {
            pending_topology_.video = *codec;
        }
    }
    else
    {
        if (pending_topology_.audio)
        {
            pending_topology_.invalid = true;
        }
        else
        {
            pending_topology_.audio = *codec;
        }
    }

    if (finish)
    {
        announced_topology_ = pending_topology_;
        pending_topology_ = {};
    }
}

bool gb28181_rtp_receiver::apply_topology(const ps_topology& topology)
{
    if (topology.invalid || !topology.video)
    {
        spdlog::warn("gb28181 unsupported ps topology stream {}", stream_name_);
        return false;
    }

    if (video_codec_)
    {
        if (video_codec_ != topology.video || audio_codec_ != topology.audio)
        {
            spdlog::warn("gb28181 ps topology change stream {}", stream_name_);
            return false;
        }
        return true;
    }

    if (topology.unsupported_audio && !topology.audio)
    {
        spdlog::warn("gb28181 unsupported audio ignored stream {}", stream_name_);
    }
    video_codec_ = topology.video;
    audio_codec_ = topology.audio;
    video_track_ = media_track{.id = video_track_id,
                               .kind = media_kind::video,
                               .codec = *video_codec_,
                               .clock_rate = 90'000,
                               .channel_count = 0,
                               .codec_config = {}};
    if (audio_codec_ == codec_id::g711a || audio_codec_ == codec_id::g711u)
    {
        audio_track_ = media_track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = *audio_codec_,
            .clock_rate = 8'000,
            .channel_count = 1,
            .codec_config = {},
        };
    }
    return true;
}

int gb28181_rtp_receiver::on_demuxed_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr)
    {
        return -1;
    }

    if (announced_topology_)
    {
        if (!apply_topology(*announced_topology_))
        {
            return -1;
        }
        announced_topology_.reset();
    }

    const auto codec = codec_from_avpacket(packet->stream->codecid);
    if (!codec && packet->stream->codecid >= AVCODEC_AUDIO_PCM && packet->stream->codecid < AVCODEC_TEXT_WEBVTT)
    {
        return 0;
    }
    if (!codec)
    {
        if (video_codec_)
        {
            spdlog::warn("gb28181 unsupported raw codec stream {} codecid {}", stream_name_, packet->stream->codecid);
            return -1;
        }
        return 0;
    }
    if (!video_codec_)
    {
        return 0;
    }
    if (is_video(*codec) ? video_codec_ != codec : audio_codec_ != codec)
    {
        spdlog::warn("gb28181 raw codec change stream {} codec {}", stream_name_, to_string(*codec));
        return -1;
    }

    auto& observed_stream = is_video(*codec) ? video_stream_ : audio_stream_;
    if (observed_stream && *observed_stream != packet->stream->stream)
    {
        spdlog::warn("gb28181 multiple {} streams {}", to_string(is_video(*codec) ? media_kind::video : media_kind::audio), stream_name_);
        return -1;
    }
    if (!observed_stream)
    {
        observed_stream = packet->stream->stream;
    }

    const auto track_result = update_track_from_packet(*packet);
    if (track_result < 0)
    {
        return -1;
    }
    if (track_result > 0)
    {
        avpkt2bs_destroy(&bitstream_);
        avpkt2bs_create(&bitstream_);
    }
    if (stream_->tracks().empty())
    {
        if (!video_codec_ || !video_track_ ||
            ((video_codec_ == codec_id::h264 || video_codec_ == codec_id::h265) && video_track_->codec_config.empty()) ||
            (audio_codec_ && !audio_track_) || (audio_codec_ == codec_id::aac && audio_track_->codec_config.empty()))
        {
            return 0;
        }

        std::vector<media_track> tracks;
        tracks.push_back(*video_track_);
        if (audio_track_)
        {
            tracks.push_back(*audio_track_);
        }
        if (!stream_->set_tracks(std::move(tracks)) || !stream_registry::instance().add(stream_))
        {
            spdlog::warn("gb28181 stream register failed {}", stream_name_);
            return -1;
        }
        video_track_.reset();
        audio_track_.reset();
        spdlog::info("gb28181 stream started {}", stream_name_);
    }

    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }

    const auto track = is_video(*codec) ? video_track_id : audio_track_id;
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    stream_->publish(media_frame{
        .track = track,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    });
    media_handler_();
    return 0;
}

int gb28181_rtp_receiver::update_track_from_packet(const avpacket_t& packet)
{
    auto track = media_track_from_avstream_config(*packet.stream, video_track_id, audio_track_id);
    if (!track)
    {
        return 0;
    }

    if (!stream_->tracks().empty())
    {
        const auto fixed = std::ranges::find_if(stream_->tracks(), [&track](const media_track& value) { return value.id == track->id; });
        if (fixed == stream_->tracks().end() || fixed->codec != track->codec || fixed->clock_rate != track->clock_rate ||
            fixed->channel_count != track->channel_count || fixed->codec_config != track->codec_config)
        {
            spdlog::warn("gb28181 track config changed stream {}", stream_name_);
            return -1;
        }
        return 0;
    }

    auto& current = track->kind == media_kind::video ? video_track_ : audio_track_;
    if (!current)
    {
        current = *track;
        return 1;
    }
    if (current->codec != track->codec)
    {
        return 0;
    }
    const bool changed =
        current->clock_rate != track->clock_rate || current->channel_count != track->channel_count || current->codec_config != track->codec_config;
    if (!changed)
    {
        return 0;
    }
    current = *track;
    return 1;
}

}    // namespace media_server
