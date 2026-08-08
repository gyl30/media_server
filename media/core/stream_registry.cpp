#include "media/core/stream_registry.h"

namespace media_server
{

bool stream_registry::add(const std::shared_ptr<media_stream>& stream)
{
    if (!stream || stream->name().empty())
    {
        return false;
    }
    return streams_.emplace(stream->name(), stream).second;
}

bool stream_registry::remove(std::string_view name, const media_stream* expected)
{
    const auto iterator = streams_.find(name);
    if (iterator == streams_.end())
    {
        return false;
    }
    if (expected && iterator->second.get() != expected)
    {
        return false;
    }
    streams_.erase(iterator);
    return true;
}

std::shared_ptr<media_stream> stream_registry::find(std::string_view name) const
{
    const auto iterator = streams_.find(name);
    return iterator == streams_.end() ? nullptr : iterator->second;
}

}    // namespace media_server
