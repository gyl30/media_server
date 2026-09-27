#include <utility>

#include "media/control/control_session_registry.h"

namespace media_server
{

control_session_registry& control_session_registry::instance()
{
    static control_session_registry registry;
    return registry;
}

bool control_session_registry::add_receiver_session(std::string stream_name, std::shared_ptr<control_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.try_emplace(std::move(stream_name)).first;
    if (iterator->second.receiver_session)
    {
        return false;
    }
    iterator->second.receiver_session = std::move(session);
    return true;
}

void control_session_registry::remove_receiver_session(std::string_view stream_name, const control_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.find(stream_name);
    if (iterator == sessions_.end() || iterator->second.receiver_session.get() != &expected)
    {
        return;
    }
    iterator->second.receiver_session.reset();
    if (empty(iterator->second))
    {
        sessions_.erase(iterator);
    }
}

bool control_session_registry::add_sender_session(std::string stream_name, std::string sender_id, std::shared_ptr<control_session> session)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.try_emplace(std::move(stream_name)).first;
    return iterator->second.sender_sessions.emplace(std::move(sender_id), std::move(session)).second;
}

std::shared_ptr<control_session> control_session_registry::take_sender_session(std::string_view stream_name,
                                                                              std::string_view sender_id,
                                                                              std::string_view expected_stream_id)
{
    std::shared_ptr<control_session> session;
    {
        std::scoped_lock lock(mutex_);
        const auto stream_iterator = sessions_.find(stream_name);
        if (stream_iterator == sessions_.end())
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
            sessions_.erase(stream_iterator);
        }
    }
    return session;
}

void control_session_registry::remove_sender_session(std::string_view stream_name, std::string_view sender_id, const control_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto stream_iterator = sessions_.find(stream_name);
    if (stream_iterator == sessions_.end())
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
        sessions_.erase(stream_iterator);
    }
}

bool control_session_registry::empty(const session_entry& entry)
{
    return !entry.receiver_session && entry.sender_sessions.empty();
}

}    // namespace media_server
