#include <utility>

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
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(expected.name());
    if (iterator == streams_.end() || iterator->second.stream.get() != &expected)
    {
        return;
    }
    iterator->second.stream.reset();
    if (empty(iterator->second))
    {
        streams_.erase(iterator);
    }
}

std::shared_ptr<media_stream> stream_registry::find(std::string_view name) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(name);
    return iterator == streams_.end() ? nullptr : iterator->second.stream;
}

bool stream_registry::add_receiver_session(std::string stream_name, std::shared_ptr<stream_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.try_emplace(std::move(stream_name)).first;
    if (iterator->second.receiver_session)
    {
        return false;
    }
    iterator->second.receiver_session = std::move(session);
    return true;
}

std::shared_ptr<stream_session> stream_registry::take_receiver_session(std::string_view stream_name)
{
    std::shared_ptr<stream_session> session;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = streams_.find(stream_name);
        if (iterator == streams_.end() || !iterator->second.receiver_session)
        {
            return {};
        }
        session = std::move(iterator->second.receiver_session);
        if (empty(iterator->second))
        {
            streams_.erase(iterator);
        }
    }
    return session;
}

void stream_registry::remove_receiver_session(std::string_view stream_name, const stream_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(stream_name);
    if (iterator == streams_.end() || iterator->second.receiver_session.get() != &expected)
    {
        return;
    }
    iterator->second.receiver_session.reset();
    if (empty(iterator->second))
    {
        streams_.erase(iterator);
    }
}

bool stream_registry::add_sender_session(std::string stream_name, std::string sender_id, std::shared_ptr<stream_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.try_emplace(std::move(stream_name)).first;
    return iterator->second.sender_sessions.emplace(std::move(sender_id), std::move(session)).second;
}

std::shared_ptr<stream_session> stream_registry::take_sender_session(std::string_view stream_name,
                                                                     std::string_view sender_id,
                                                                     std::string_view expected_stream_id)
{
    std::shared_ptr<stream_session> session;
    {
        std::scoped_lock lock(mutex_);
        const auto stream_iterator = streams_.find(stream_name);
        if (stream_iterator == streams_.end())
        {
            return {};
        }
        const auto sender_iterator = stream_iterator->second.sender_sessions.find(sender_id);
        if (sender_iterator == stream_iterator->second.sender_sessions.end() ||
            (!expected_stream_id.empty() && sender_iterator->second->stream_id() != expected_stream_id))
        {
            return {};
        }
        session = std::move(sender_iterator->second);
        stream_iterator->second.sender_sessions.erase(sender_iterator);
        if (empty(stream_iterator->second))
        {
            streams_.erase(stream_iterator);
        }
    }
    return session;
}

void stream_registry::remove_sender_session(std::string_view stream_name, std::string_view sender_id, const stream_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto stream_iterator = streams_.find(stream_name);
    if (stream_iterator == streams_.end())
    {
        return;
    }
    const auto sender_iterator = stream_iterator->second.sender_sessions.find(sender_id);
    if (sender_iterator == stream_iterator->second.sender_sessions.end() || sender_iterator->second.get() != &expected)
    {
        return;
    }
    sender_iterator->second.reset();
    stream_iterator->second.sender_sessions.erase(sender_iterator);
    if (empty(stream_iterator->second))
    {
        streams_.erase(stream_iterator);
    }
}

bool stream_registry::empty(const stream_entry& entry) { return !entry.stream && !entry.receiver_session && entry.sender_sessions.empty(); }

}    // namespace media_server
