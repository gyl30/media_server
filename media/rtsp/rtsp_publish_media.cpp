#include <bit>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/scope/scope_exit.hpp>

#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_publish_media.h"

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
constexpr char rtcp_name[] = "media_server";
}    // namespace

rtsp_publish_media::rtsp_publish_media(worker_context& worker,
                                       std::string media_stream_name,
                                       std::vector<rtsp_publish_track_description> descriptions,
                                       std::function<void()> media_handler)
    : worker_(worker),
      media_stream_name_(std::move(media_stream_name)),
      descriptions_(std::move(descriptions)),
      media_handler_(std::move(media_handler))
{
}

bool rtsp_publish_media::startup(const std::string& rtcp_cname)
{
    if (descriptions_.empty())
    {
        return false;
    }

    media_stream_ = std::make_shared<media_stream>(media_stream_name_, worker_);
    avpkt2bs_create(&bitstream_);
    demuxers_.resize(descriptions_.size());
    for (std::size_t index = 0; index < descriptions_.size(); ++index)
    {
        const auto& description = descriptions_[index];
        auto* demuxer = rtsp_demuxer_create(static_cast<int>(index), 500, &rtsp_publish_media::packet_callback, this);
        boost::scope::scope_exit cleanup_demuxer(
            [&]()
            {
                if (demuxer != nullptr)
                {
                    rtsp_demuxer_destroy(demuxer);
                }
            });
        if (demuxer == nullptr ||
            rtsp_demuxer_add_payload(demuxer,
                                     description.clock_rate,
                                     description.payload_type,
                                     description.encoding.c_str(),
                                     description.fmtp.empty() ? nullptr : description.fmtp.c_str()) != 0 ||
            rtsp_demuxer_set_info(demuxer, rtcp_cname.c_str(), rtcp_name) != 0)
        {
            shutdown();
            return false;
        }
        demuxers_[index] = demuxer;
        cleanup_demuxer.set_active(false);
    }
    return true;
}

bool rtsp_publish_media::start_recording()
{
    if (!media_stream_ || !media_stream_->tracks().empty())
    {
        return false;
    }

    std::vector<media_track> tracks;
    tracks.reserve(descriptions_.size());
    for (const auto& description : descriptions_)
    {
        tracks.push_back(description.track);
    }
    std::ranges::sort(tracks, [](const media_track& left, const media_track& right) { return left.id < right.id; });
    if (!media_stream_->set_tracks(std::move(tracks)))
    {
        return false;
    }
    if (!stream_registry::instance().add(media_stream_))
    {
        media_stream_->end();
        media_stream_.reset();
        return false;
    }
    return true;
}

bool rtsp_publish_media::input_packet(std::size_t track_index, std::span<const std::uint8_t> data)
{
    if (!media_stream_)
    {
        return false;
    }
    if (media_stream_->tracks().empty() || track_index >= demuxers_.size() || data.size() < 4)
    {
        return true;
    }
    auto* demuxer = demuxers_[track_index];
    if (demuxer != nullptr)
    {
        const auto result = rtsp_demuxer_input(demuxer, data.data(), static_cast<int>(data.size()));
        if (result == RTCP_SR)
        {
            std::uint32_t ntp_msw{};
            std::uint32_t ntp_lsw{};
            std::uint32_t rtp_timestamp{};
            std::int64_t pts{};
            if (rtsp_demuxer_sender_report(demuxer, &ntp_msw, &ntp_lsw, &rtp_timestamp, &pts) == 0)
            {
                const auto ntp = (static_cast<std::uint64_t>(ntp_msw) << 32U) | ntp_lsw;
                if (!rtcp_sync_)
                {
                    rtcp_sync_ = rtcp_sync{.ntp = ntp, .pts = pts};
                }
                else
                {
                    constexpr std::int64_t ntp_fraction = std::int64_t{1} << 32U;
                    // NTP is unsigned 32.32 fixed-point; interpret modular subtraction as the signed session-time delta.
                    const auto delta = std::bit_cast<std::int64_t>(ntp - rtcp_sync_->ntp);
                    pts = rtcp_sync_->pts + (delta / ntp_fraction) * 1'000 + (delta % ntp_fraction) * 1'000 / ntp_fraction;
                }
                rtsp_demuxer_set_timestamp(demuxer, rtp_timestamp, pts);
            }
        }
        else if (result < 0)
        {
            return false;
        }
    }
    return true;
}

int rtsp_publish_media::generate_rtcp(std::size_t track_index, std::span<std::uint8_t> buffer)
{
    if (!media_stream_ || media_stream_->tracks().empty() || track_index >= demuxers_.size() || demuxers_[track_index] == nullptr)
    {
        return 0;
    }
    return rtsp_demuxer_rtcp(demuxers_[track_index], buffer.data(), static_cast<int>(buffer.size()));
}

void rtsp_publish_media::shutdown()
{
    if (media_stream_)
    {
        stream_registry::instance().remove(*media_stream_);
        media_stream_->end();
        media_stream_.reset();
    }
    for (auto*& demuxer : demuxers_)
    {
        if (demuxer != nullptr)
        {
            rtsp_demuxer_destroy(demuxer);
            demuxer = nullptr;
        }
    }
    demuxers_.clear();
    rtcp_sync_.reset();
    avpkt2bs_destroy(&bitstream_);
}

const std::vector<rtsp_publish_track_description>& rtsp_publish_media::descriptions() const noexcept { return descriptions_; }

const std::string& rtsp_publish_media::media_stream_name() const noexcept { return media_stream_name_; }


int rtsp_publish_media::packet_callback(void* param, avpacket_t* packet)
{
    return static_cast<rtsp_publish_media*>(param)->on_demuxed_packet(packet);
}

int rtsp_publish_media::on_demuxed_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || !media_stream_ || media_stream_->tracks().empty())
    {
        return -1;
    }

    const auto codecid = packet->stream->codecid;
    const bool video = codecid == AVCODEC_VIDEO_H264 || codecid == AVCODEC_VIDEO_H265;
    const bool audio =
        codecid == AVCODEC_AUDIO_AAC || codecid == AVCODEC_AUDIO_OPUS || codecid == AVCODEC_AUDIO_G711A || codecid == AVCODEC_AUDIO_G711U;
    const auto id = video ? video_track_id : (audio ? audio_track_id : 0);
    if (id == 0)
    {
        return 0;
    }
    const auto codec = codecid == AVCODEC_VIDEO_H264    ? codec_id::h264
                       : codecid == AVCODEC_VIDEO_H265  ? codec_id::h265
                       : codecid == AVCODEC_AUDIO_AAC   ? codec_id::aac
                       : codecid == AVCODEC_AUDIO_OPUS  ? codec_id::opus
                       : codecid == AVCODEC_AUDIO_G711A ? codec_id::g711a
                                                        : codec_id::g711u;
    const auto state = std::find_if(
        descriptions_.begin(), descriptions_.end(), [codec](const rtsp_publish_track_description& value) { return value.track.codec == codec; });
    if (state == descriptions_.end())
    {
        spdlog::warn("rtsp publish raw codec change {}", to_string(codec));
        return -1;
    }

    if (!validate_track_from_packet(*packet))
    {
        return -1;
    }
    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    media_stream_->publish(media_frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    });
    media_handler_();
    return 0;
}

bool rtsp_publish_media::validate_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    auto track = media_track_from_avstream_config(input, video_track_id, audio_track_id);
    if (!track && input.codecid == AVCODEC_AUDIO_OPUS && input.sample_rate == 48'000 && (input.channels == 1 || input.channels == 2))
    {
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = codec_id::opus,
                            .clock_rate = 48'000,
                            .channel_count = static_cast<std::uint16_t>(input.channels),
                            .codec_config = {}};
    }
    if (!track)
    {
        return false;
    }

    const auto state = std::find_if(descriptions_.begin(),
                                    descriptions_.end(),
                                    [track](const rtsp_publish_track_description& value) { return value.track.id == track->id; });
    if (state == descriptions_.end())
    {
        return false;
    }
    if (state->track.codec != track->codec || state->track.clock_rate != track->clock_rate || state->track.channel_count != track->channel_count ||
        state->track.codec_config != track->codec_config)
    {
        spdlog::warn("rtsp publish track config changed {}", to_string(track->codec));
        return false;
    }
    return true;
}

}    // namespace media_server
