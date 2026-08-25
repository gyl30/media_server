#include "media/core/stream_registry.h"

namespace media_server
{

bool stream_registry::add(const std::shared_ptr<media_stream>& stream)
{
    if (!stream || stream->name().empty() || stream->tracks().empty())
    {
        return false;
    }

    std::scoped_lock lock(mutex_);
    return streams_.emplace(stream->name(), stream).second;
}

void stream_registry::remove(const media_stream& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(expected.name());
    if (iterator != streams_.end() && iterator->second.get() == &expected)
    {
        streams_.erase(iterator);
    }
}

std::shared_ptr<media_stream> stream_registry::find(std::string_view name) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(name);
    return iterator == streams_.end() ? nullptr : iterator->second;
}

void stream_registry::clear()
{
    std::scoped_lock lock(mutex_);
    streams_.clear();
}

}    // namespace media_server
