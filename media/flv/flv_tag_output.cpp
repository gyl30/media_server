#include <array>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>

#include "media/core/media_stream.h"
#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/flv/flv_tag_output.h"

extern "C"
{
#include "flv-proto.h"
#include "flv-muxer.h"
#include "flv-header.h"
#include "opus-head.h"
}

namespace media_server
{
namespace
{
bool sequence_header(int type, const std::uint8_t* data, std::size_t bytes)
{
    if (type == FLV_TYPE_VIDEO)
    {
        flv_video_tag_header_t video{};
        return flv_video_tag_header_read(&video, data, bytes) >= 0 && video.avpacket == FLV_SEQUENCE_HEADER;
    }
    flv_audio_tag_header_t audio{};
    return flv_audio_tag_header_read(&audio, data, bytes) >= 0 && audio.avpacket == FLV_SEQUENCE_HEADER;
}
}    // namespace

flv_tag_output::flv_tag_output(worker_context& worker)
    : worker_(worker), muxer_(nullptr, &flv_muxer_destroy), headers_(std::make_shared<flv_headers>()), dispatcher_(worker)
{
}

worker_context& flv_tag_output::worker() noexcept { return worker_; }

bool flv_tag_output::startup(const std::shared_ptr<media_stream>& source)
{
    muxer_.reset(flv_muxer_create(&flv_tag_output::on_packet, this));
    if (!muxer_)
    {
        spdlog::error("flv muxer create failed stream {}", source->stream_id());
        return false;
    }
    for (const auto& track : source->tracks())
    {
        has_video_ = has_video_ || track.kind == media_kind::video;
        if (!write_track_config(track))
        {
            return false;
        }
        codecs_.emplace(track.id, track.codec);
    }
    source_ = source;
    source->add_sink(shared_from_this());
    return true;
}

bool flv_tag_output::write_track_config(const media_track& track)
{
    if ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) &&
        (track.clock_rate != 8'000 || track.channel_count != 1 || !track.codec_config.empty()))
    {
        return false;
    }
    if (track.codec == codec_id::opus &&
        (track.clock_rate != 48'000 || (track.channel_count != 1 && track.channel_count != 2) || !track.codec_config.empty()))
    {
        return false;
    }
    if (track.codec == codec_id::h265)
    {
        flv_muxer_set_enhanced_rtmp(muxer_.get(), 1);
    }
    if (track.codec_config.empty() && track.codec != codec_id::opus)
    {
        return true;
    }

    int result = 0;
    if (track.codec == codec_id::h264)
    {
        result = flv_muxer_avc(muxer_.get(), track.codec_config.data(), track.codec_config.size(), 0, 0);
    }
    else if (track.codec == codec_id::h265)
    {
        result = flv_muxer_hevc(muxer_.get(), track.codec_config.data(), track.codec_config.size(), 0, 0);
    }
    else if (track.codec == codec_id::opus)
    {
        std::array<std::uint8_t, 29> head_data{};
        const opus_head_t head{
            .version = 1,
            .channels = static_cast<std::uint8_t>(track.channel_count),
            .pre_skip = 0,
            .input_sample_rate = 48'000,
            .output_gain = 0,
            .channel_mapping_family = 0,
            .stream_count = 0,
            .coupled_count = 0,
            .channel_mapping = {},
        };
        const auto bytes = opus_head_save(&head, head_data.data(), head_data.size());
        if (bytes <= 0)
        {
            spdlog::error("flv opus head create failed track {}", track.id);
            return false;
        }
        result = flv_muxer_opus(muxer_.get(), head_data.data(), static_cast<std::size_t>(bytes), 0, 0);
    }
    else
    {
        return true;
    }

    if (result != 0)
    {
        spdlog::error("flv write track config codec {} result {}", to_string(track.codec), result);
        return false;
    }
    return true;
}

void flv_tag_output::on_frame(const media_frame& frame)
{
    if (!muxer_ || !frame.payload)
    {
        return;
    }
    const auto iterator = codecs_.find(frame.track);
    if (iterator == codecs_.end())
    {
        return;
    }

    const auto pts = ns_to_flv_milliseconds(frame.pts_ns);
    const auto dts = ns_to_flv_milliseconds(frame.dts_ns);
    auto* muxer = muxer_.get();
    key_frame_ = frame.key_frame;
    int result = -1;
    switch (iterator->second)
    {
        case codec_id::h264:
            result = flv_muxer_avc(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::h265:
            result = flv_muxer_hevc(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::aac:
            result = flv_muxer_aac(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::opus:
            result = flv_muxer_opus(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::g711a:
            result = flv_muxer_g711a(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
        case codec_id::g711u:
            result = flv_muxer_g711u(muxer, frame.payload->data(), frame.payload->size(), pts, dts);
            break;
    }
    if (result != 0)
    {
        spdlog::error("flv mux failed stream {} track {} result {}", source_->stream_id(), frame.track, result);
        finish();
    }
}

void flv_tag_output::on_end() { finish(); }

void flv_tag_output::add_sink(std::shared_ptr<flv_sink> sink)
{
    auto* target = &sink->worker();
    boost::asio::dispatch(worker_.io(),
                          [self = shared_from_this(), sink = std::move(sink), target]() mutable
                          {
                              if (!self->source_ || !self->muxer_)
                              {
                                  boost::asio::post(target->io(), [sink = std::move(sink)]() { sink->on_end(); });
                                  return;
                              }
                              self->dispatcher_.add(std::move(sink), *target);
                          });
}

void flv_tag_output::finish()
{
    if (source_)
    {
        source_->remove_sink(this);
        source_.reset();
    }
    dispatcher_.end();
    muxer_.reset();
}

int flv_tag_output::on_packet(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto& self = *static_cast<flv_tag_output*>(param);
    const auto* begin = static_cast<const std::uint8_t*>(data);
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(begin, begin + bytes);
    const bool header = sequence_header(type, begin, bytes);
    if (header)
    {
        auto headers = std::make_shared<flv_headers>(*self.headers_);
        (type == FLV_TYPE_VIDEO ? headers->video : headers->audio) = payload;
        self.headers_ = std::move(headers);
    }
    const bool key_frame = !header && (type == FLV_TYPE_VIDEO ? self.key_frame_ : !self.has_video_);
    self.dispatcher_.publish({.type = type, .key_frame = key_frame, .data = std::move(payload), .timestamp = timestamp, .headers = self.headers_});
    return 0;
}

}    // namespace media_server
