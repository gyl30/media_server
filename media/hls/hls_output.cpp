#include "media/hls/hls_output.h"

#include "media/codec/codec_utils.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "mpeg-ts.h"
#include "mpeg-proto.h"
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace media_server
{

hls_output::hls_output(double target_duration_seconds, std::size_t window_size)
    : target_duration_seconds_(target_duration_seconds), window_size_(window_size)
{
    recreate_muxer();
}

hls_output::~hls_output()
{
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
    }
}

void hls_output::on_track(const media_track& track)
{
    tracks_.insert_or_assign(track.id, track);
    if (track.kind == media_kind::video)
    {
        has_video_ = true;
        if (segment_start_pts_ns_ < 0 && current_segment_.empty())
        {
            waiting_for_key_frame_ = true;
        }
    }
    if (muxer_ != nullptr)
    {
        const auto stream_id = add_track_to_muxer(track);
        if (stream_id > 0)
        {
            stream_ids_.insert_or_assign(track.id, stream_id);
        }
    }
}

void hls_output::on_frame(const media_frame& frame)
{
    if (ended_ || muxer_ == nullptr || !frame.payload)
    {
        return;
    }

    const auto track_iterator = tracks_.find(frame.track);
    const auto stream_iterator = stream_ids_.find(frame.track);
    if (track_iterator == tracks_.end() || stream_iterator == stream_ids_.end())
    {
        return;
    }

    if (waiting_for_key_frame_)

    {
        if (track_iterator->second.kind != media_kind::video || !frame.key_frame)
        {
            return;
        }
        waiting_for_key_frame_ = false;
    }

    if (segment_start_pts_ns_ < 0)

    {
        segment_start_pts_ns_ = frame.pts_ns;
    }

    const auto elapsed_ns = frame.pts_ns - segment_start_pts_ns_;
    const auto target_ns = static_cast<std::int64_t>(target_duration_seconds_ * 1'000'000'000.0);
    if (track_iterator->second.kind == media_kind::video &&
        frame.key_frame &&
        elapsed_ns >= target_ns &&
        !current_segment_.empty())
        {
        finish_segment(frame.pts_ns);
        segment_start_pts_ns_ = frame.pts_ns;
    }

    const auto flags = frame.key_frame ? 1 : 0;
    const auto result = mpeg_ts_write(
        muxer_,
        stream_iterator->second,
        flags,
        ns_to_90khz(frame.pts_ns),
        ns_to_90khz(frame.dts_ns),
        frame.payload->data(),
        frame.payload->size());

    if (result != 0)

    {
        spdlog::error("hls ts write failed track {} result {}", frame.track, result);
    }
    last_pts_ns_ = frame.pts_ns;
}

void hls_output::on_end()
{
    if (ended_)
    {
        return;
    }
    if (!current_segment_.empty())
    {
        finish_segment(last_pts_ns_);
    }
    ended_ = true;
}

std::string hls_output::playlist(std::string_view base_path) const
{
    std::ostringstream output;
    output << "#EXTM3U\n";
    output << "#EXT-X-VERSION:3\n";
    double maximum_duration = target_duration_seconds_;
    for (const auto& item : segments_)
    {
        maximum_duration = std::max(maximum_duration, item.duration);
    }
    output << "#EXT-X-TARGETDURATION:" << std::max(1, static_cast<int>(std::ceil(maximum_duration))) << "\n";
    const auto first_sequence = segments_.empty() ? next_sequence_ : segments_.front().sequence;
    output << "#EXT-X-MEDIA-SEQUENCE:" << first_sequence << "\n";

    for (const auto& item : segments_)

    {
        output << "#EXTINF:" << std::fixed << std::setprecision(3) << item.duration << ",\n";
        output << base_path << '/' << item.sequence << ".ts\n";
    }

    if (ended_)

    {
        output << "#EXT-X-ENDLIST\n";
    }
    return output.str();
}

std::optional<std::vector<std::uint8_t>> hls_output::segment(std::uint64_t sequence) const
{
    const auto iterator = std::find_if(
        segments_.begin(), segments_.end(),
        [sequence](const hls_segment& item) { return item.sequence == sequence; });
    if (iterator == segments_.end())
    {
        return std::nullopt;
    }
    return iterator->data;
}

std::size_t hls_output::segment_count() const noexcept
{
    return segments_.size();
}

void* hls_output::ts_alloc(void*, std::size_t bytes)
{
    return std::malloc(bytes);
}

void hls_output::ts_free(void*, void* packet)
{
    std::free(packet);
}

int hls_output::ts_write(void* param, const void* packet, std::size_t bytes)
{
    auto* self = static_cast<hls_output*>(param);
    const auto* begin = static_cast<const std::uint8_t*>(packet);
    self->current_segment_.insert(self->current_segment_.end(), begin, begin + bytes);
    return 0;
}

void hls_output::recreate_muxer()
{
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
    }

    const mpeg_ts_func_t functions{
        .alloc = &hls_output::ts_alloc,
        .free = &hls_output::ts_free,
        .write = &hls_output::ts_write,
    };
    muxer_ = mpeg_ts_create(&functions, this);
    stream_ids_.clear();

    for (const auto& [id, track] : tracks_)

    {
        const auto stream_id = add_track_to_muxer(track);
        if (stream_id > 0)
        {
            stream_ids_.insert_or_assign(id, stream_id);
        }
    }
}

void hls_output::finish_segment(std::int64_t end_pts_ns)
{
    if (current_segment_.empty())
    {
        return;
    }

    double duration = target_duration_seconds_;
    if (segment_start_pts_ns_ >= 0 && end_pts_ns >= segment_start_pts_ns_)
    {
        duration = static_cast<double>(end_pts_ns - segment_start_pts_ns_) / 1'000'000'000.0;
    }
    if (duration <= 0.0)
    {
        duration = target_duration_seconds_;
    }

    segments_.push_back(hls_segment{
        .sequence = next_sequence_++,
        .duration = duration,
        .data = std::move(current_segment_),
    });
    current_segment_.clear();

    while (segments_.size() > window_size_)

    {
        segments_.pop_front();
    }

    recreate_muxer();
}

int hls_output::add_track_to_muxer(const media_track& track)
{
    switch (track.codec)
    {
    case codec_id::h264:
        return mpeg_ts_add_stream(muxer_, PSI_STREAM_H264, nullptr, 0);
    case codec_id::aac:
        return mpeg_ts_add_stream(muxer_, PSI_STREAM_AAC, nullptr, 0);
    }
    return -1;
}

}    // namespace media_server
