#include <utility>

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
    const auto iterator = streams_.try_emplace(stream->name()).first;
    if (iterator->second.stream)
    {
        return false;
    }
    iterator->second.stream = stream;
    return true;
}

void stream_registry::remove(const media_stream& expected)
{
    std::shared_ptr<media_stream> removed;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = streams_.find(expected.name());
        if (iterator == streams_.end() || iterator->second.stream.get() != &expected)
        {
            return;
        }
        removed = std::move(iterator->second.stream);
        if (empty(iterator->second))
        {
            streams_.erase(iterator);
        }
    }
}

std::shared_ptr<media_stream> stream_registry::find(std::string_view name) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(name);
    return iterator == streams_.end() ? nullptr : iterator->second.stream;
}

bool stream_registry::add_input_session(std::string stream_name, std::shared_ptr<stream_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.try_emplace(std::move(stream_name)).first;
    if (iterator->second.input_session)
    {
        return false;
    }
    iterator->second.input_session = std::move(session);
    return true;
}

std::shared_ptr<stream_session> stream_registry::take_input_session(std::string_view stream_name)
{
    std::shared_ptr<stream_session> session;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = streams_.find(stream_name);
        if (iterator == streams_.end() || !iterator->second.input_session)
        {
            return {};
        }
        session = std::move(iterator->second.input_session);
        if (empty(iterator->second))
        {
            streams_.erase(iterator);
        }
    }
    return session;
}

void stream_registry::remove_input_session(std::string_view stream_name, const stream_session& expected)
{
    std::shared_ptr<stream_session> removed;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = streams_.find(stream_name);
        if (iterator == streams_.end() || iterator->second.input_session.get() != &expected)
        {
            return;
        }
        removed = std::move(iterator->second.input_session);
        if (empty(iterator->second))
        {
            streams_.erase(iterator);
        }
    }
}

bool stream_registry::add_output_session(std::string stream_name, std::string output_id, std::shared_ptr<stream_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.try_emplace(std::move(stream_name)).first;
    return iterator->second.output_sessions.emplace(std::move(output_id), std::move(session)).second;
}

std::shared_ptr<stream_session> stream_registry::take_output_session(std::string_view stream_name, std::string_view output_id)
{
    std::shared_ptr<stream_session> session;
    {
        std::scoped_lock lock(mutex_);
        const auto stream_iterator = streams_.find(stream_name);
        if (stream_iterator == streams_.end())
        {
            return {};
        }
        const auto output_iterator = stream_iterator->second.output_sessions.find(output_id);
        if (output_iterator == stream_iterator->second.output_sessions.end())
        {
            return {};
        }
        session = std::move(output_iterator->second);
        stream_iterator->second.output_sessions.erase(output_iterator);
        if (empty(stream_iterator->second))
        {
            streams_.erase(stream_iterator);
        }
    }
    return session;
}

void stream_registry::remove_output_session(std::string_view stream_name, std::string_view output_id, const stream_session& expected)
{
    std::shared_ptr<stream_session> removed;
    {
        std::scoped_lock lock(mutex_);
        const auto stream_iterator = streams_.find(stream_name);
        if (stream_iterator == streams_.end())
        {
            return;
        }
        const auto output_iterator = stream_iterator->second.output_sessions.find(output_id);
        if (output_iterator == stream_iterator->second.output_sessions.end() || output_iterator->second.get() != &expected)
        {
            return;
        }
        removed = std::move(output_iterator->second);
        stream_iterator->second.output_sessions.erase(output_iterator);
        if (empty(stream_iterator->second))
        {
            streams_.erase(stream_iterator);
        }
    }
}

void stream_registry::clear()
{
    // 先在锁内摘除全局引用，再在锁外释放对象，避免在 registry mutex 内执行析构。
    std::map<std::string, stream_entry, std::less<>> streams;
    {
        std::scoped_lock lock(mutex_);
        streams.swap(streams_);
    }
}

bool stream_registry::empty(const stream_entry& entry) { return !entry.stream && !entry.input_session && entry.output_sessions.empty(); }

}    // namespace media_server
