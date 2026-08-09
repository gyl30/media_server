#include "media/core/media_stream.h"

#include <algorithm>
#include <utility>

namespace media_server
{

media_stream::media_stream(std::string name)
    : name_(std::move(name))
{
}

const std::string& media_stream::name() const noexcept
{
    return name_;
}

bool media_stream::ended() const noexcept
{
    return ended_;
}

std::vector<media_track> media_stream::tracks() const
{
    std::vector<media_track> result;
    result.reserve(tracks_.size());
    for (const auto& [id, track] : tracks_)
    {
        static_cast<void>(id);
        result.push_back(track);
    }
    return result;
}

bool media_stream::add_sink(const std::shared_ptr<media_sink>& sink)
{
    if (!sink || ended_)
    {
        return false;
    }

    remove_expired_sinks();
    for (const auto& weak_sink : sinks_)
    {
        if (const auto current = weak_sink.lock(); current && current.get() == sink.get())
        {
            return false;
        }
    }

    // 先挂接再重放轨道配置，保证 on_track 中 remove_sink/end 的重入语义有效。
    const auto track_snapshot = tracks();
    sinks_.push_back(sink);
    for (const auto& track : track_snapshot)
    {
        sink->on_track(track);
        if (ended_)
        {
            return false;
        }

        const auto attached = std::any_of(sinks_.begin(), sinks_.end(), [sink_ptr = sink.get()](const std::weak_ptr<media_sink>& weak_sink) {
            const auto current = weak_sink.lock();
            return current && current.get() == sink_ptr;
        });
        if (!attached)
        {
            return false;
        }
    }
    return true;
}

bool media_stream::remove_sink(const media_sink* sink)
{
    if (!sink)
    {
        return false;
    }

    const auto old_size = sinks_.size();
    std::erase_if(sinks_, [sink](const std::weak_ptr<media_sink>& weak_sink) {
        const auto current = weak_sink.lock();
        return !current || current.get() == sink;
    });
    return sinks_.size() != old_size;
}

bool media_stream::update_track(media_track track)
{
    if (ended_ || track.id == 0 || track.codec_config.empty())
    {
        return false;
    }

    tracks_[track.id] = track;
    for (const auto& sink : sink_snapshot())
    {
        sink->on_track(track);
    }
    return true;
}

bool media_stream::publish(media_frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty() || !tracks_.contains(frame.track))
    {
        return false;
    }

    for (const auto& sink : sink_snapshot())

    {
        sink->on_frame(frame);
    }
    return true;
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }

    ended_ = true;
    const auto sinks = sink_snapshot();
    sinks_.clear();
    for (const auto& sink : sinks)
    {
        sink->on_end();
    }
}

std::vector<std::shared_ptr<media_sink>> media_stream::sink_snapshot()
{
    remove_expired_sinks();
    std::vector<std::shared_ptr<media_sink>> result;
    result.reserve(sinks_.size());
    for (const auto& weak_sink : sinks_)
    {
        if (const auto sink = weak_sink.lock())
        {
            result.push_back(sink);
        }
    }
    return result;
}

void media_stream::remove_expired_sinks()
{
    std::erase_if(sinks_, [](const std::weak_ptr<media_sink>& weak_sink) {
        return weak_sink.expired();
    });
}

}    // namespace media_server
