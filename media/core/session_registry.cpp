#include <utility>

#include "media/core/session_registry.h"

namespace media_server
{

session_registry& session_registry::instance()
{
    static session_registry registry;
    return registry;
}

bool session_registry::add_receiver_session(std::string stream_id, std::shared_ptr<session> session)
{
    std::scoped_lock lock(mutex_);
    if (stopping_)
    {
        return false;
    }
    const auto iterator = sessions_.try_emplace(std::move(stream_id)).first;
    if (iterator->second.receiver)
    {
        return false;
    }
    iterator->second.receiver = std::move(session);
    return true;
}

std::shared_ptr<session> session_registry::take_receiver_session(std::string_view stream_id)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.find(stream_id);
    if (iterator == sessions_.end() || !iterator->second.receiver)
    {
        return {};
    }
    auto value = std::move(iterator->second.receiver);
    if (empty(iterator->second))
    {
        sessions_.erase(iterator);
    }
    return value;
}

void session_registry::remove_receiver_session(std::string_view stream_id, const session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.find(stream_id);
    if (iterator == sessions_.end() || !iterator->second.receiver || iterator->second.receiver.get() != &expected)
    {
        return;
    }
    iterator->second.receiver.reset();
    if (empty(iterator->second))
    {
        sessions_.erase(iterator);
    }
}

std::shared_ptr<session> session_registry::find_receiver_session(std::string_view stream_id) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = sessions_.find(stream_id);
    if (iterator == sessions_.end() || !iterator->second.receiver)
    {
        return {};
    }
    return iterator->second.receiver;
}

std::vector<std::string> session_registry::receivers() const
{
    std::scoped_lock lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [stream_id, entry] : sessions_)
    {
        if (entry.receiver)
        {
            result.push_back(stream_id);
        }
    }
    return result;
}

bool session_registry::add_sender_session(std::string stream_id, std::string sender_id, std::shared_ptr<session> session)
{
    std::scoped_lock lock(mutex_);
    if (stopping_)
    {
        return false;
    }
    const auto iterator = sessions_.try_emplace(std::move(stream_id)).first;
    return iterator->second.sender_sessions.emplace(std::move(sender_id), std::move(session)).second;
}

std::shared_ptr<session> session_registry::take_sender_session(std::string_view stream_id, std::string_view sender_id)
{
    std::shared_ptr<session> value;
    {
        std::scoped_lock lock(mutex_);
        const auto stream_iterator = sessions_.find(stream_id);
        if (stream_iterator == sessions_.end())
        {
            return {};
        }
        const auto sender_iterator = stream_iterator->second.sender_sessions.find(sender_id);
        if (sender_iterator == stream_iterator->second.sender_sessions.end())
        {
            return {};
        }
        value = std::move(sender_iterator->second);
        stream_iterator->second.sender_sessions.erase(sender_iterator);
        if (empty(stream_iterator->second))
        {
            sessions_.erase(stream_iterator);
        }
    }
    return value;
}

void session_registry::remove_sender_session(std::string_view stream_id, std::string_view sender_id, const session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto stream_iterator = sessions_.find(stream_id);
    if (stream_iterator == sessions_.end())
    {
        return;
    }
    const auto sender_iterator = stream_iterator->second.sender_sessions.find(sender_id);
    if (sender_iterator == stream_iterator->second.sender_sessions.end() || sender_iterator->second.get() != &expected)
    {
        return;
    }
    stream_iterator->second.sender_sessions.erase(sender_iterator);
    if (empty(stream_iterator->second))
    {
        sessions_.erase(stream_iterator);
    }
}

void session_registry::shutdown_all()
{
    std::map<std::string, session_entry, std::less<>> detached;
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        detached.swap(sessions_);
    }
    for (const auto& [stream_id, entry] : detached)
    {
        if (entry.receiver)
        {
            entry.receiver->shutdown();
        }
        for (const auto& [sender_id, session] : entry.sender_sessions)
        {
            session->shutdown();
        }
    }
}

bool session_registry::empty(const session_entry& entry)
{
    return !entry.receiver && entry.sender_sessions.empty();
}

}    // namespace media_server
