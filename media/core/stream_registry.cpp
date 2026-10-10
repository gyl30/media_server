#include "media/core/stream_registry.h"

namespace media_server
{

stream_registry& stream_registry::instance()
{
    static stream_registry registry;
    return registry;
}

bool stream_registry::add(const std::shared_ptr<media_stream>& stream)
{
    if (!stream || stream->stream_id().empty() || stream->tracks().empty())
    {
        return false;
    }

    std::scoped_lock lock(mutex_);
    return streams_.emplace(stream->stream_id(), stream).second;
}

void stream_registry::remove(const media_stream& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(expected.stream_id());
    if (iterator == streams_.end() || iterator->second.get() != &expected)
    {
        return;
    }
    streams_.erase(iterator);
}

std::shared_ptr<media_stream> stream_registry::find(std::string_view stream_id) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(stream_id);
    return iterator == streams_.end() ? nullptr : iterator->second;
}

}    // namespace media_server
