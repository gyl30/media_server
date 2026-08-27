#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_input_media.h"

extern "C"
{
#include "avpacket.h"
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

rtsp_input_media::rtsp_input_media(boost::asio::any_io_executor executor,
                                   std::string stream_name,
                                   std::vector<rtsp_input_track_description> descriptions)
    : executor_(std::move(executor)), stream_name_(std::move(stream_name)), descriptions_(std::move(descriptions))
{
}

rtsp_input_media::~rtsp_input_media() = default;

bool rtsp_input_media::startup(const std::string& rtcp_cname)
{
    if (closed_ || stream_ || descriptions_.empty())
    {
        return false;
    }

    stream_ = std::make_shared<media_stream>(stream_name_, executor_);
    static_cast<void>(avpkt2bs_create(&bitstream_));
    demuxers_.resize(descriptions_.size());
    for (std::size_t index = 0; index < descriptions_.size(); ++index)
    {
        const auto& description = descriptions_[index];
        auto* demuxer = rtsp_demuxer_create(static_cast<int>(index), 500, &rtsp_input_media::packet_callback, this);
        if (demuxer == nullptr ||
            rtsp_demuxer_add_payload(demuxer,
                                     description.clock_rate,
                                     description.payload_type,
                                     description.encoding.c_str(),
                                     description.fmtp.empty() ? nullptr : description.fmtp.c_str()) != 0 ||
            rtsp_demuxer_set_info(demuxer, rtcp_cname.c_str(), rtcp_name) != 0)
        {
            if (demuxer != nullptr)
            {
                rtsp_demuxer_destroy(demuxer);
            }
            shutdown();
            return false;
        }
        demuxers_[index] = demuxer;
    }
    return true;
}

bool rtsp_input_media::start_recording()
{
    if (closed_ || recording_ || !stream_)
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
    if (!stream_->set_tracks(std::move(tracks)) || !registry::instance().add(stream_))
    {
        return false;
    }
    recording_ = true;
    return true;
}

bool rtsp_input_media::input(std::size_t track_index, std::span<const std::uint8_t> data)
{
    if (closed_ || !recording_ || track_index >= demuxers_.size() || data.size() < 4)
    {
        return !closed_;
    }
    auto* demuxer = demuxers_[track_index];
    if (demuxer != nullptr)
    {
        static_cast<void>(rtsp_demuxer_input(demuxer, data.data(), static_cast<int>(data.size())));
    }
    return !fatal_codec_change_;
}

int rtsp_input_media::rtcp(std::size_t track_index, std::span<std::uint8_t> buffer)
{
    if (closed_ || !recording_ || track_index >= demuxers_.size() || demuxers_[track_index] == nullptr)
    {
        return 0;
    }
    return rtsp_demuxer_rtcp(demuxers_[track_index], buffer.data(), static_cast<int>(buffer.size()));
}

void rtsp_input_media::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_)
    {
        registry::instance().remove(*stream_);
        stream_->end();
        stream_.reset();
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

const std::vector<rtsp_input_track_description>& rtsp_input_media::descriptions() const noexcept { return descriptions_; }

const std::string& rtsp_input_media::stream_name() const noexcept { return stream_name_; }

bool rtsp_input_media::recording() const noexcept { return recording_; }

int rtsp_input_media::packet_callback(void* param, avpacket_t* packet) { return static_cast<rtsp_input_media*>(param)->on_packet(packet); }

int rtsp_input_media::on_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || !recording_ || closed_ || !stream_)
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
        descriptions_.begin(), descriptions_.end(), [codec](const rtsp_input_track_description& value) { return value.track.codec == codec; });
    if (state == descriptions_.end())
    {
        spdlog::warn("rtsp publish raw codec change {}", to_string(codec));
        fatal_codec_change_ = true;
        return -1;
    }

    if (update_track_from_packet(*packet))
    {
        avpkt2bs_destroy(&bitstream_);
        static_cast<void>(avpkt2bs_create(&bitstream_));
    }
    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    stream_->publish(media_frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    });
    return 0;
}

bool rtsp_input_media::update_track_from_packet(const avpacket_t& packet)
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
    else if ((input.codecid == AVCODEC_AUDIO_G711A || input.codecid == AVCODEC_AUDIO_G711U) && input.sample_rate == 8'000 && input.channels == 1)
    {
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = input.codecid == AVCODEC_AUDIO_G711A ? codec_id::g711a : codec_id::g711u,
                            .clock_rate = 8'000,
                            .channel_count = 1,
                            .codec_config = {}};
    }
    else if (input.codecid == AVCODEC_AUDIO_OPUS && input.sample_rate == 48'000 && (input.channels == 1 || input.channels == 2))
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

    const auto state = std::find_if(
        descriptions_.begin(), descriptions_.end(), [track](const rtsp_input_track_description& value) { return value.track.codec == track->codec; });
    if (state == descriptions_.end() || !stream_->update_track(*track))
    {
        return false;
    }
    state->track = std::move(*track);
    return true;
}

}    // namespace media_server
