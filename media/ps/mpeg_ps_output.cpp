#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include "media/ps/mpeg_ps_output.h"
#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/codec/codec_utils.h"

extern "C"
{
#include "mpeg-ps.h"
}

namespace media_server
{
namespace
{
constexpr std::size_t max_pending_frames = 2500;
}

struct mpeg_ps_output::sink_group
{
    worker_context& worker;
    std::mutex mutex;
    std::vector<std::weak_ptr<mpeg_ps_sink>> sinks;
    std::deque<mpeg_ps_frame> pending;
    bool drain_queued{};
    bool pending_end{};

    explicit sink_group(worker_context& target) : worker(target) {}

    [[nodiscard]] std::vector<std::shared_ptr<mpeg_ps_sink>> snapshot_sinks()
    {
        std::scoped_lock lock(mutex);
        std::vector<std::shared_ptr<mpeg_ps_sink>> result;
        std::erase_if(sinks, [](const auto& sink) { return sink.expired(); });
        for (const auto& weak : sinks)
        {
            if (auto sink = weak.lock())
            {
                result.push_back(std::move(sink));
            }
        }
        return result;
    }

    void drain()
    {
        for (;;)
        {
            std::deque<mpeg_ps_frame> frames;
            bool finish = false;
            {
                std::scoped_lock lock(mutex);
                frames.swap(pending);
                if (frames.empty() && pending_end)
                {
                    pending_end = false;
                    drain_queued = false;
                    finish = true;
                }
                else if (frames.empty())
                {
                    drain_queued = false;
                    return;
                }
            }
            const auto sinks_snapshot = snapshot_sinks();
            for (const auto& frame : frames)
            {
                for (const auto& sink : sinks_snapshot)
                {
                    sink->on_ps_frame(frame);
                }
            }
            if (finish)
            {
                for (const auto& sink : sinks_snapshot)
                {
                    sink->on_end();
                }
                return;
            }
        }
    }
};

mpeg_ps_output::mpeg_ps_output(worker_context& worker) : worker_(worker), muxer_(nullptr, &ps_muxer_destroy)
{
}

worker_context& mpeg_ps_output::worker() noexcept { return worker_; }

bool mpeg_ps_output::supported_tracks(const std::vector<media_track>& tracks)
{
    std::size_t video_count = 0;
    std::size_t audio_count = 0;
    for (const auto& track : tracks)
    {
        if (track.kind == media_kind::video)
        {
            ++video_count;
            if (track.codec != codec_id::h264 && track.codec != codec_id::h265)
            {
                return false;
            }
        }
        else
        {
            ++audio_count;
            if (track.codec != codec_id::aac && track.codec != codec_id::g711a && track.codec != codec_id::g711u)
            {
                return false;
            }
        }
    }
    return video_count == 1 && audio_count <= 1;
}

bool mpeg_ps_output::startup(const std::shared_ptr<media_stream>& source)
{
    if (muxer_ || !source || !supported_tracks(source->tracks()))
    {
        return false;
    }
    source_ = source;
    const ps_muxer_func_t callbacks{allocate_packet, free_packet, write_packet};
    muxer_.reset(ps_muxer_create(&callbacks, this));
    if (!muxer_)
    {
        spdlog::error("mpeg ps muxer create failed stream {}", source->name());
        finish();
        return false;
    }
    for (const auto& track : source->tracks())
    {
        int codec = 0;
        switch (track.codec)
        {
            case codec_id::h264:
                codec = PSI_STREAM_H264;
                break;
            case codec_id::h265:
                codec = PSI_STREAM_H265;
                break;
            case codec_id::aac:
                codec = PSI_STREAM_AAC;
                break;
            case codec_id::g711a:
                codec = PSI_STREAM_AUDIO_G711A;
                break;
            case codec_id::g711u:
                codec = PSI_STREAM_AUDIO_G711U;
                break;
            case codec_id::opus:
                break;
        }
        const auto id = ps_muxer_add_stream(muxer_.get(), codec, nullptr, 0);
        if (id < 0)
        {
            spdlog::error("mpeg ps muxer add track failed stream {} track {}", source->name(), track.id);
            finish();
            return false;
        }
        mux_tracks_.emplace(track.id, std::pair{track.kind, id});
    }
    source->add_sink(shared_from_this());
    return true;
}

void mpeg_ps_output::on_frame(const media_frame& frame)
{
    if (!muxer_ || !frame.payload)
    {
        return;
    }
    const auto iterator = mux_tracks_.find(frame.track);
    if (iterator == mux_tracks_.end())
    {
        return;
    }
    const auto& [kind, id] = iterator->second;
    if (waiting_for_key_frame_ && (kind != media_kind::video || !frame.key_frame))
    {
        return;
    }
    const auto pts = ns_to_milliseconds(frame.pts_ns) * 90;
    if (ps_muxer_input(muxer_.get(), id, frame.key_frame ? MPEG_FLAG_IDR_FRAME : 0, pts, ns_to_milliseconds(frame.dts_ns) * 90,
                       frame.payload->data(), frame.payload->size()) < 0)
    {
        spdlog::error("mpeg ps muxer input failed stream {} track {}", source_->name(), frame.track);
        finish();
        return;
    }
    waiting_for_key_frame_ = false;
    publish({.track = frame.track,
             .dts_ns = frame.dts_ns,
             .pts_ns = frame.pts_ns,
             .key_frame = frame.key_frame,
             .payload = std::move(packet_),
             .media_timestamp = static_cast<std::uint32_t>(pts)});
}

void mpeg_ps_output::on_end() { finish(); }

void mpeg_ps_output::add_sink(std::shared_ptr<mpeg_ps_sink> sink)
{
    if (!sink)
    {
        return;
    }
    auto* target = &sink->worker();
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink = std::move(sink), target]() mutable
                          { self->add_sink_owner(std::move(sink), *target); });
}

void mpeg_ps_output::add_sink_owner(std::shared_ptr<mpeg_ps_sink> sink, worker_context& worker)
{
    if (!source_ || !muxer_)
    {
        boost::asio::post(worker.io(), [sink = std::move(sink)]() { sink->on_end(); });
        return;
    }
    auto& group = sink_groups_[&worker];
    if (!group)
    {
        group = std::make_shared<sink_group>(worker);
    }
    std::scoped_lock lock(group->mutex);
    group->sinks.emplace_back(std::move(sink));
}

void mpeg_ps_output::remove_sink(mpeg_ps_sink* sink)
{
    if (!sink)
    {
        return;
    }
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink]() { self->remove_sink_owner(sink); });
}

void mpeg_ps_output::remove_sink_owner(mpeg_ps_sink* sink)
{
    for (auto iterator = sink_groups_.begin(); iterator != sink_groups_.end();)
    {
        const auto& group = iterator->second;
        {
            std::scoped_lock lock(group->mutex);
            std::erase_if(group->sinks, [sink](const auto& weak)
                          {
                              const auto current = weak.lock();
                              return !current || current.get() == sink;
                          });
        }
        if (group->snapshot_sinks().empty())
        {
            iterator = sink_groups_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

void mpeg_ps_output::publish(mpeg_ps_frame frame)
{
    std::vector<std::shared_ptr<sink_group>> groups;
    groups.reserve(sink_groups_.size());
    for (const auto& [worker, group] : sink_groups_)
    {
        static_cast<void>(worker);
        groups.push_back(group);
    }
    std::vector<std::shared_ptr<sink_group>> overflowed_groups;
    for (const auto& group : groups)
    {
        if (&group->worker == &worker_)
        {
            for (const auto& sink : group->snapshot_sinks())
            {
                sink->on_ps_frame(frame);
            }
            continue;
        }
        bool schedule = false;
        {
            std::scoped_lock lock(group->mutex);
            if (group->pending_end)
            {
                continue;
            }
            if (group->pending.size() >= max_pending_frames)
            {
                group->pending.clear();
                group->pending_end = true;
                overflowed_groups.push_back(group);
            }
            else
            {
                group->pending.push_back(frame);
            }
            if (!group->drain_queued)
            {
                group->drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            boost::asio::post(group->worker.io(), [group]() { group->drain(); });
        }
    }
    for (const auto& overflowed : overflowed_groups)
    {
        for (auto iterator = sink_groups_.begin(); iterator != sink_groups_.end();)
        {
            if (iterator->second == overflowed)
            {
                iterator = sink_groups_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }
}

void mpeg_ps_output::end_sinks()
{
    std::vector<std::shared_ptr<sink_group>> groups;
    groups.reserve(sink_groups_.size());
    for (const auto& [worker, group] : sink_groups_)
    {
        static_cast<void>(worker);
        groups.push_back(group);
    }
    sink_groups_.clear();
    for (const auto& group : groups)
    {
        if (&group->worker == &worker_)
        {
            for (const auto& sink : group->snapshot_sinks())
            {
                sink->on_end();
            }
            continue;
        }
        bool schedule = false;
        {
            std::scoped_lock lock(group->mutex);
            group->pending_end = true;
            if (!group->drain_queued)
            {
                group->drain_queued = true;
                schedule = true;
            }
        }
        if (schedule)
        {
            boost::asio::post(group->worker.io(), [group]() { group->drain(); });
        }
    }
}

void mpeg_ps_output::finish()
{
    if (source_)
    {
        source_->remove_sink(this);
        source_.reset();
    }
    end_sinks();
    packet_.reset();
    muxer_.reset();
}

void* mpeg_ps_output::allocate_packet(void* param, std::size_t bytes)
{
    auto& self = *static_cast<mpeg_ps_output*>(param);
    self.packet_ = std::make_shared<std::vector<std::uint8_t>>(bytes);
    return self.packet_->data();
}

void mpeg_ps_output::free_packet(void*, void*) {}

int mpeg_ps_output::write_packet(void* param, int, void*, std::size_t bytes)
{
    static_cast<mpeg_ps_output*>(param)->packet_->resize(bytes);
    return 0;
}

}    // namespace media_server
