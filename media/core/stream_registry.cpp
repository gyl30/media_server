#include "media/core/stream_registry.h"

namespace media_server
{

bool stream_registry::add(const std::shared_ptr<media_stream>& stream)
{
    if (!stream || stream->name().empty() || stream->ended())
    {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (stream->ended())
    {
        return false;
    }
    const auto iterator = streams_.find(stream->name());
    if (iterator == streams_.end())
    {
        streams_.emplace(stream->name(), stream);
        return true;
    }
    if (!iterator->second->ended())
    {
        return false;
    }

    iterator->second = stream;
    return true;
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
    if (iterator == streams_.end() || iterator->second->ended())
    {
        return {};
    }
    return iterator->second;
}

}    // namespace media_server
