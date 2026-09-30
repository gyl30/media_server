#include <algorithm>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/scope/scope_exit.hpp>

#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_pull_media.h"

extern "C"
{
#include "avpacket.h"
#include "avstream.h"
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

rtsp_pull_media::rtsp_pull_media(worker_context& worker, std::string media_stream_name, std::vector<rtsp_pull_track_description> descriptions)
    : worker_(worker), media_stream_name_(std::move(media_stream_name)), descriptions_(std::move(descriptions))
{
}

bool rtsp_pull_media::startup()
{
    if (media_stream_ || descriptions_.empty())
    {
        return false;
    }

    media_stream_ = std::make_shared<media_stream>(media_stream_name_, worker_);
    avpkt2bs_create(&bitstream_);
    demuxers_.resize(descriptions_.size());
    for (std::size_t index = 0; index < descriptions_.size(); ++index)
    {
        auto& description = descriptions_[index];
        expected_audio_ = expected_audio_ || description.kind == media_kind::audio;
        if (description.initial_track)
        {
            auto& pending = description.kind == media_kind::video ? initial_video_track_ : initial_audio_track_;
            pending = std::move(*description.initial_track);
        }

        auto* demuxer = rtsp_demuxer_create(static_cast<int>(index), 500, &rtsp_pull_media::packet_callback, this);
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
            rtsp_demuxer_set_info(demuxer, media_stream_name_.c_str(), rtcp_name) != 0)
        {
            shutdown();
            return false;
        }
        demuxers_[index] = demuxer;
        cleanup_demuxer.set_active(false);
    }
    return true;
}

bool rtsp_pull_media::input_packet(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (!media_stream_)
    {
        return false;
    }

    const auto media = static_cast<std::size_t>(channel / 2U);
    const bool rtcp = (channel % 2U) != 0U;
    if (media >= demuxers_.size() || demuxers_[media] == nullptr || data.size() < (rtcp ? 4U : 12U))
    {
        return true;
    }

    if (!rtcp && media_stream_->tracks().empty())
    {
        if (register_stream_if_ready() < 0)
        {
            return false;
        }
    }
    if (rtsp_demuxer_input(demuxers_[media], data.data(), static_cast<int>(data.size())) < 0)
    {
        return false;
    }
    return true;
}

int rtsp_pull_media::set_rtp_info(std::size_t media, std::uint16_t sequence, std::uint32_t timestamp)
{
    if (media >= demuxers_.size() || demuxers_[media] == nullptr)
    {
        return -1;
    }
    return rtsp_demuxer_rtpinfo(demuxers_[media], sequence, timestamp);
}

int rtsp_pull_media::generate_rtcp(std::size_t media, std::span<std::uint8_t> buffer)
{
    if (media >= demuxers_.size() || demuxers_[media] == nullptr)
    {
        return 0;
    }
    return rtsp_demuxer_rtcp(demuxers_[media], buffer.data(), static_cast<int>(buffer.size()));
}

bool rtsp_pull_media::has_tracks() const noexcept { return media_stream_ && !media_stream_->tracks().empty(); }

void rtsp_pull_media::shutdown()
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
    avpkt2bs_destroy(&bitstream_);
}

int rtsp_pull_media::packet_callback(void* param, avpacket_t* packet) { return static_cast<rtsp_pull_media*>(param)->on_demuxed_packet(packet); }

int rtsp_pull_media::on_demuxed_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || !media_stream_)
    {
        return -1;
    }

    // avpkt2bs 会缓存首次解析的编解码配置；ready 后配置变化结束当前 source generation。
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
    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }

    track_id id{};
    if (packet->stream->codecid == AVCODEC_VIDEO_H264 || packet->stream->codecid == AVCODEC_VIDEO_H265)
    {
        id = video_track_id;
    }
    else if (packet->stream->codecid == AVCODEC_AUDIO_AAC || packet->stream->codecid == AVCODEC_AUDIO_OPUS ||
             packet->stream->codecid == AVCODEC_AUDIO_G711A || packet->stream->codecid == AVCODEC_AUDIO_G711U)
    {
        id = audio_track_id;
    }
    else
    {
        return 0;
    }

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    media_stream_->publish(media_frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    });
    return 0;
}

int rtsp_pull_media::update_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    auto track = media_track_from_avstream_config(input, video_track_id, audio_track_id);
    if (!track)
    {
        return 0;
    }

    if (!media_stream_->tracks().empty())
    {
        const auto fixed = std::ranges::find_if(media_stream_->tracks(), [&track](const media_track& value) { return value.id == track->id; });
        if (fixed == media_stream_->tracks().end() || fixed->codec != track->codec || fixed->clock_rate != track->clock_rate ||
            fixed->channel_count != track->channel_count || fixed->codec_config != track->codec_config)
        {
            spdlog::warn("rtsp pull track config changed {} {}", to_string(track->kind), to_string(track->codec));
            return -1;
        }
        return 0;
    }

    bool changed = false;
    auto& pending = track->kind == media_kind::video ? initial_video_track_ : initial_audio_track_;
    if (pending)
    {
        changed = pending->codec != track->codec || pending->clock_rate != track->clock_rate || pending->channel_count != track->channel_count ||
                  pending->codec_config != track->codec_config;
    }
    pending = *track;
    if (register_stream_if_ready() < 0)
    {
        return -1;
    }
    return changed ? 1 : 0;
}

int rtsp_pull_media::register_stream_if_ready()
{
    if (!media_stream_->tracks().empty() || !initial_video_track_ || (expected_audio_ && !initial_audio_track_))
    {
        return 0;
    }

    std::vector<media_track> tracks;
    tracks.push_back(*initial_video_track_);
    if (expected_audio_)
    {
        tracks.push_back(*initial_audio_track_);
    }
    if (!media_stream_->set_tracks(std::move(tracks)))
    {
        return -1;
    }
    if (!stream_registry::instance().add(media_stream_))
    {
        spdlog::warn("rtsp pull duplicate stream {}", media_stream_name_);
        return -1;
    }
    initial_video_track_.reset();
    initial_audio_track_.reset();
    spdlog::info("rtsp pull tracks ready audio {}", expected_audio_);
    return 0;
}

}    // namespace media_server
