#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <spdlog/spdlog.h>

#include "media/hls/hls_segmenter.h"
#include "media/codec/codec_utils.h"
#include "media/core/media_stream.h"
#include "media/net/worker_context.h"

extern "C"
{
#include "mpeg-proto.h"
#include "mpeg-ts.h"
}

namespace media_server
{
namespace
{
constexpr std::size_t max_segment_bytes = 16U * 1024U * 1024U;
}

hls_segmenter::hls_segmenter(hls_config config)
    : target_duration_seconds_(config.target_duration_seconds), window_size_(config.window_size)
{
    recreate_muxer();
}

void hls_segmenter::process_frame(const media_frame& frame)
{
    std::scoped_lock lock(mutex_);
    if (ended_at_.has_value() || !frame.payload)
    {
        return;
    }
    const auto track_iterator = tracks_.find(frame.track);
    if (track_iterator == tracks_.end())
    {
        return;
    }
    const auto& track = track_iterator->second;
    if (waiting_for_key_frame_)
    {
        if (track.kind != media_kind::video || !frame.key_frame)
        {
            return;
        }
        waiting_for_key_frame_ = false;
    }
    if (muxer_ == nullptr)
    {
        return;
    }
    if (!segment_start_pts_ns_)
    {
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }
    const auto elapsed_ns = frame.pts_ns - *segment_start_pts_ns_;
    const auto target_ns = static_cast<std::int64_t>(target_duration_seconds_ * 1'000'000'000.0);
    const bool segment_boundary = track.kind == media_kind::video && frame.key_frame && elapsed_ns >= target_ns;
    if (segment_boundary && !current_segment_.empty())
    {
        finish_segment(frame.pts_ns);
        recreate_muxer();
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }
    const auto stream_iterator = stream_ids_.find(frame.track);
    if (stream_iterator == stream_ids_.end())
    {
        return;
    }
    const auto flags = frame.key_frame ? 1 : 0;
    const auto result = mpeg_ts_write(
        muxer_, stream_iterator->second, flags, ns_to_90khz(frame.pts_ns), ns_to_90khz(frame.dts_ns), frame.payload->data(), frame.payload->size());
    if (result != 0)
    {
        if (result == -ENOBUFS)
        {
            discard_segment();
            return;
        }
        spdlog::error("hls ts write failed track {} result {}", frame.track, result);
    }
    segment_max_pts_ns_ = std::max(segment_max_pts_ns_, frame.pts_ns);
}

void hls_segmenter::finish()
{
    std::scoped_lock lock(mutex_);
    if (ended_at_.has_value())
    {
        return;
    }
    if (!current_segment_.empty())
    {
        finish_segment(segment_max_pts_ns_);
    }
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
        muxer_ = nullptr;
        stream_ids_.clear();
    }
    ended_at_ = std::chrono::steady_clock::now();
}

bool hls_segmenter::startup(const std::shared_ptr<media_stream>& source)
{
    if (!source || source_)
    {
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        for (const auto& track : source->tracks())
        {
            tracks_.emplace(track.id, track);
            const auto stream_id = add_track_to_muxer(track);
            if (stream_id < 0)
            {
                return false;
            }
            stream_ids_.insert_or_assign(track.id, stream_id);
        }
    }
    source_ = source;
    source->add_sink(shared_from_this());
    return true;
}

worker_context& hls_segmenter::worker() noexcept { return source_->worker(); }

void hls_segmenter::on_frame(const media_frame& frame) { process_frame(frame); }

void hls_segmenter::on_end() { finish(); }

void hls_segmenter::shutdown()
{
    if (source_)
    {
        source_->remove_sink(this);
        source_.reset();
    }
    finish();
}

std::string hls_segmenter::playlist(std::string_view base_path, std::string_view query) const
{
    std::scoped_lock lock(mutex_);
    std::ostringstream output;
    output << "#EXTM3U\n#EXT-X-VERSION:3\n";
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
        output << base_path << '/' << item.sequence << ".ts";
        if (!query.empty())
        {
            output << '?' << query;
        }
        output << '\n';
    }
    if (ended_at_.has_value())
    {
        output << "#EXT-X-ENDLIST\n";
    }
    return output.str();
}

std::optional<std::vector<std::uint8_t>> hls_segmenter::segment(std::uint64_t sequence) const
{
    const auto buffer = segment_buffer(sequence);
    return buffer ? std::optional<std::vector<std::uint8_t>>(*buffer) : std::nullopt;
}

std::shared_ptr<const std::vector<std::uint8_t>> hls_segmenter::segment_buffer(std::uint64_t sequence) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = std::find_if(segments_.begin(), segments_.end(), [sequence](const hls_segment& item) { return item.sequence == sequence; });
    return iterator == segments_.end() ? std::shared_ptr<const std::vector<std::uint8_t>>{} : iterator->data;
}

std::size_t hls_segmenter::segment_count() const
{
    std::scoped_lock lock(mutex_);
    return segments_.size();
}

std::optional<std::chrono::steady_clock::time_point> hls_segmenter::ended_at() const
{
    std::scoped_lock lock(mutex_);
    return ended_at_;
}

void* hls_segmenter::ts_alloc(void*, std::size_t bytes) { return std::malloc(bytes); }

void hls_segmenter::ts_free(void*, void* packet) { std::free(packet); }

int hls_segmenter::ts_write(void* param, const void* packet, std::size_t bytes)
{
    auto* self = static_cast<hls_segmenter*>(param);
    if (bytes > max_segment_bytes - self->current_segment_.size())
    {
        return -ENOBUFS;
    }
    const auto* begin = static_cast<const std::uint8_t*>(packet);
    self->current_segment_.insert(self->current_segment_.end(), begin, begin + bytes);
    return 0;
}

void hls_segmenter::recreate_muxer()
{
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
    }
    const mpeg_ts_func_t functions{
        .alloc = &hls_segmenter::ts_alloc,
        .free = &hls_segmenter::ts_free,
        .write = &hls_segmenter::ts_write,
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

void hls_segmenter::finish_segment(std::int64_t end_pts_ns)
{
    if (current_segment_.empty())
    {
        return;
    }
    double duration = target_duration_seconds_;
    if (segment_start_pts_ns_ && end_pts_ns >= *segment_start_pts_ns_)
    {
        duration = static_cast<double>(end_pts_ns - *segment_start_pts_ns_) / 1'000'000'000.0;
    }
    if (duration <= 0.0)
    {
        duration = target_duration_seconds_;
    }
    segments_.push_back(hls_segment{
        .sequence = next_sequence_++,
        .duration = duration,
        .data = std::make_shared<const std::vector<std::uint8_t>>(std::move(current_segment_)),
    });
    current_segment_.clear();
    while (segments_.size() > window_size_)
    {
        segments_.pop_front();
    }
}

void hls_segmenter::discard_segment()
{
    spdlog::warn("hls discarding unfinished segment {}", next_sequence_);
    recreate_muxer();
    std::vector<std::uint8_t>().swap(current_segment_);
    segment_start_pts_ns_.reset();
    segment_max_pts_ns_ = 0;
    waiting_for_key_frame_ = true;
}

int hls_segmenter::add_track_to_muxer(const media_track& track)
{
    switch (track.codec)
    {
        case codec_id::h264:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_H264, nullptr, 0);
        case codec_id::h265:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_H265, nullptr, 0);
        case codec_id::aac:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_AAC, nullptr, 0);
        case codec_id::g711a:
            return track.clock_rate == 8'000 && track.channel_count == 1 && track.codec_config.empty()
                       ? mpeg_ts_add_stream(muxer_, PSI_STREAM_AUDIO_G711A, nullptr, 0)
                       : -1;
        case codec_id::g711u:
            return track.clock_rate == 8'000 && track.channel_count == 1 && track.codec_config.empty()
                       ? mpeg_ts_add_stream(muxer_, PSI_STREAM_AUDIO_G711U, nullptr, 0)
                       : -1;
        case codec_id::opus:
            return -1;
    }
    return -1;
}

}    // namespace media_server
