#include <utility>

#include "media/control/control_session_registry.h"

namespace media_server
{

control_session_registry& control_session_registry::instance()
{
    static control_session_registry registry;
    return registry;
}

bool control_session_registry::add_receiver_session(std::string stream_name, std::string stream_id, std::shared_ptr<control_session> session)
{
    std::scoped_lock lock(mutex_);
    if (stopping_)
    {
        return false;
    }
    const auto iterator = sessions_.try_emplace(std::move(stream_name)).first;
    if (iterator->second.receiver_session.session)
    {
        return false;
    }
    iterator->second.receiver_session = {.stream_id = std::move(stream_id), .session = std::move(session)};
    return true;
}

void control_session_registry::remove_receiver_session(std::string_view stream_name, const control_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.find(stream_name);
    if (iterator == sessions_.end() || iterator->second.receiver_session.session.get() != &expected)
    {
        return;
    }
    iterator->second.receiver_session = {};
    if (empty(iterator->second))
    {
        sessions_.erase(iterator);
    }
}

bool control_session_registry::add_sender_session(
    std::string stream_name, std::string sender_id, std::string stream_id, std::shared_ptr<control_session> session)
{
    std::scoped_lock lock(mutex_);
    if (stopping_)
    {
        return false;
    }
    const auto iterator = sessions_.try_emplace(std::move(stream_name)).first;
    return iterator->second.sender_sessions
        .emplace(std::move(sender_id), registered_session{.stream_id = std::move(stream_id), .session = std::move(session)})
        .second;
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
        if (sender_iterator == stream_iterator->second.sender_sessions.end() || sender_iterator->second.stream_id != expected_stream_id)
        {
            return {};
        }
        session = std::move(sender_iterator->second.session);
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
    if (sender_iterator == stream_iterator->second.sender_sessions.end() || sender_iterator->second.session.get() != &expected)
    {
        return;
    }
    stream_iterator->second.sender_sessions.erase(sender_iterator);
    if (empty(stream_iterator->second))
    {
        sessions_.erase(stream_iterator);
    }
}

void control_session_registry::shutdown_all()
{
    std::map<std::string, session_entry, std::less<>> detached;
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        detached.swap(sessions_);
    }
    for (const auto& [stream_name, entry] : detached)
    {
        if (entry.receiver_session.session)
        {
            entry.receiver_session.session->shutdown();
        }
        for (const auto& [sender_id, session] : entry.sender_sessions)
        {
            session.session->shutdown();
        }
    }
}

bool control_session_registry::empty(const session_entry& entry)
{
    return !entry.receiver_session.session && entry.sender_sessions.empty();
}

}    // namespace media_server
