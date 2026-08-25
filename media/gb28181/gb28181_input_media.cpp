#include <vector>
#include <utility>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/gb28181/gb28181_input_media.h"

extern "C"
{
#include "avpacket.h"
#include "mpeg-proto.h"
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

gb28181_input_media::gb28181_input_media(
    stream_registry& registry_ref, boost::asio::any_io_executor executor, std::string stream_name, std::uint8_t payload_type, std::uint32_t expected_ssrc)
    : registry_(registry_ref),
      executor_(std::move(executor)),
      stream_name_(std::move(stream_name)),
      payload_type_(payload_type),
      expected_ssrc_(expected_ssrc)
{
}

gb28181_input_media::~gb28181_input_media() { shutdown(); }

bool gb28181_input_media::startup()
{
    if (closed_ || demuxer_ != nullptr || stream_name_.empty())
    {
        return false;
    }

    stream_ = std::make_shared<media_stream>(stream_name_, executor_);
    static_cast<void>(avpkt2bs_create(&bitstream_));
    demuxer_ = rtsp_demuxer_create(0, 500, &gb28181_input_media::packet_callback, this);
    if (demuxer_ == nullptr || rtsp_demuxer_add_payload(demuxer_, 90'000, payload_type_, "PS", nullptr) != 0 ||
        rtsp_demuxer_set_ps_notify(demuxer_, &gb28181_input_media::stream_callback, this) != 0 ||
        rtsp_demuxer_set_info(demuxer_, stream_name_.c_str(), "media_server") != 0)
    {
        shutdown();
        return false;
    }
    return true;
}

gb28181_rtp_input_result gb28181_input_media::input_rtp(std::span<const std::uint8_t> data)
{
    if (closed_)
    {
        return gb28181_rtp_input_result::fatal;
    }
    if (demuxer_ == nullptr || data.size() < 12)
    {
        return gb28181_rtp_input_result::ignored;
    }

    rtp_packet_t packet{};
    if (rtp_packet_deserialize(&packet, data.data(), static_cast<int>(data.size())) != 0 || packet.rtp.pt != payload_type_ ||
        packet.rtp.ssrc != expected_ssrc_)
    {
        return gb28181_rtp_input_result::ignored;
    }

    static_cast<void>(rtsp_demuxer_input(demuxer_, data.data(), static_cast<int>(data.size())));
    return fatal_codec_change_ ? gb28181_rtp_input_result::fatal : gb28181_rtp_input_result::accepted;
}

int gb28181_input_media::input_rtcp(std::span<const std::uint8_t> data)
{
    if (closed_ || demuxer_ == nullptr || data.size() < 4)
    {
        return -1;
    }
    return rtsp_demuxer_input(demuxer_, data.data(), static_cast<int>(data.size()));
}

int gb28181_input_media::rtcp(std::span<std::uint8_t> buffer)
{
    if (closed_ || demuxer_ == nullptr)
    {
        return 0;
    }
    return rtsp_demuxer_rtcp(demuxer_, buffer.data(), static_cast<int>(buffer.size()));
}

void gb28181_input_media::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_)
    {
        registry_.remove(*stream_);
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

const std::string& gb28181_input_media::stream_name() const noexcept { return stream_name_; }

int gb28181_input_media::packet_callback(void* param, avpacket_t* packet) { return static_cast<gb28181_input_media*>(param)->on_packet(packet); }

void gb28181_input_media::stream_callback(void* param, int stream, int codecid, const void* extra, int bytes, int finish)
{
    static_cast<void>(stream);
    static_cast<void>(extra);
    static_cast<void>(bytes);
    static_cast<gb28181_input_media*>(param)->on_stream(codecid, finish != 0);
}

void gb28181_input_media::on_stream(int codecid, bool finish)
{
    if (closed_ || fatal_codec_change_)
    {
        return;
    }
    if (!collecting_topology_)
    {
        pending_topology_ = {};
        collecting_topology_ = true;
    }

    const auto codec = codec_from_ps(codecid);
    if (!codec)
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
        collecting_topology_ = false;
        apply_topology();
    }
}

void gb28181_input_media::apply_topology()
{
    if (pending_topology_.invalid || !pending_topology_.video)
    {
        spdlog::warn("gb28181 unsupported ps topology stream {}", stream_name_);
        fatal_codec_change_ = true;
        return;
    }

    if (topology_known_)
    {
        if (video_codec_ != pending_topology_.video || audio_codec_ != pending_topology_.audio)
        {
            spdlog::warn("gb28181 ps topology change stream {}", stream_name_);
            fatal_codec_change_ = true;
        }
        return;
    }

    video_codec_ = pending_topology_.video;
    audio_codec_ = pending_topology_.audio;
    video_track_ = media_track{.id = video_track_id,
                               .kind = media_kind::video,
                               .codec = *video_codec_,
                               .clock_rate = 90'000,
                               .channel_count = 0,
                               .codec_config = {},
                               .config_version = 0};
    if (audio_codec_ == codec_id::g711a || audio_codec_ == codec_id::g711u)
    {
        audio_track_ = media_track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = *audio_codec_,
            .clock_rate = 8'000,
            .channel_count = 1,
            .codec_config = {},
            .config_version = 0,
        };
    }
    topology_known_ = true;
    static_cast<void>(try_start_recording());
}

int gb28181_input_media::on_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || closed_ || fatal_codec_change_)
    {
        return -1;
    }

    const auto codec = codec_from_avpacket(packet->stream->codecid);
    if (!codec)
    {
        if (topology_known_)
        {
            spdlog::warn("gb28181 unsupported raw codec stream {} codecid {}", stream_name_, packet->stream->codecid);
            fatal_codec_change_ = true;
            return -1;
        }
        return 0;
    }
    if (!topology_known_)
    {
        return 0;
    }
    if (is_video(*codec) ? video_codec_ != codec : audio_codec_ != codec)
    {
        spdlog::warn("gb28181 raw codec change stream {} codec {}", stream_name_, to_string(*codec));
        fatal_codec_change_ = true;
        return -1;
    }

    if (update_track_from_packet(*packet))
    {
        avpkt2bs_destroy(&bitstream_);
        static_cast<void>(avpkt2bs_create(&bitstream_));
    }
    if (!recording_ && !try_start_recording())
    {
        return fatal_codec_change_ ? -1 : 0;
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
    return 0;
}

bool gb28181_input_media::update_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    std::optional<media_track> track;
    if (input.codecid == AVCODEC_VIDEO_H264 && input.extra != nullptr && input.bytes > 0)
    {
        auto config =
            h264_avcc_to_annex_b(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        if (!config.empty())
        {
            track = media_track{
                .id = video_track_id, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (input.codecid == AVCODEC_VIDEO_H265 && input.extra != nullptr && input.bytes > 0)
    {
        auto config =
            h265_hvcc_to_annex_b(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        if (!config.empty())
        {
            track = media_track{
                .id = video_track_id, .kind = media_kind::video, .codec = codec_id::h265, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (input.codecid == AVCODEC_AUDIO_AAC && input.extra != nullptr && input.bytes > 0 && input.sample_rate > 0 && input.channels > 0)
    {
        const auto* begin = static_cast<const std::uint8_t*>(input.extra);
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = codec_id::aac,
                            .clock_rate = static_cast<std::uint32_t>(input.sample_rate),
                            .channel_count = static_cast<std::uint16_t>(input.channels),
                            .codec_config = std::vector<std::uint8_t>(begin, begin + input.bytes)};
    }
    if (!track)
    {
        return false;
    }

    auto& current = track->kind == media_kind::video ? video_track_ : audio_track_;
    if (!current)
    {
        current = *track;
        return true;
    }
    if (current->codec != track->codec)
    {
        return false;
    }
    const bool changed =
        current->clock_rate != track->clock_rate || current->channel_count != track->channel_count || current->codec_config != track->codec_config;
    if (!changed)
    {
        return false;
    }
    current = *track;
    if (recording_)
    {
        static_cast<void>(stream_->update_track(std::move(*track)));
    }
    return true;
}

bool gb28181_input_media::try_start_recording()
{
    if (recording_)
    {
        return true;
    }
    if (!topology_known_ || !video_track_ || (audio_codec_ && !audio_track_))
    {
        return false;
    }

    std::vector<media_track> tracks;
    tracks.push_back(*video_track_);
    if (audio_track_)
    {
        tracks.push_back(*audio_track_);
    }
    if (!stream_->set_tracks(std::move(tracks)) || !registry_.add(stream_))
    {
        spdlog::warn("gb28181 stream register failed {}", stream_name_);
        fatal_codec_change_ = true;
        return false;
    }
    recording_ = true;
    spdlog::info("gb28181 stream started {}", stream_name_);
    return true;
}

}    // namespace media_server
