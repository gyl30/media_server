#include "media/gb28181/gb28181_service.h"

#include "media/gb28181/gb28181_sdp.h"
#include "media/gb28181/gb28181_udp_session.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace media_server
{

gb28181_service::gb28181_service(stream_registry& registry) : registry_(registry) {}

gb28181_service::~gb28181_service() { shutdown(); }

gb28181_create_error gb28181_service::create(boost::asio::any_io_executor executor, std::string stream_name, std::string_view sdp)
{
    if (stream_name.empty())
    {
        return gb28181_create_error::invalid_sdp;
    }

    auto description = parse_gb28181_udp_sdp(sdp);
    if (!description)
    {
        return gb28181_create_error::invalid_sdp;
    }
    if (registry_.find(stream_name))
    {
        return gb28181_create_error::stream_conflict;
    }

    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return gb28181_create_error::internal_error;
        }
        std::erase_if(sessions_, [](const auto& entry) { return entry.second.expired(); });
        const auto iterator = sessions_.find(stream_name);
        if (iterator != sessions_.end())
        {
            return gb28181_create_error::duplicate_stream;
        }
    }

    auto session = std::make_shared<gb28181_udp_session>(registry_, std::move(executor), stream_name, std::move(*description));
    if (!session->startup())
    {
        return gb28181_create_error::internal_error;
    }

    bool inserted = false;
    bool closed = false;
    {
        std::scoped_lock lock(sessions_mutex_);
        closed = closed_;
        if (!closed)
        {
            std::erase_if(sessions_, [](const auto& entry) { return entry.second.expired(); });
            inserted = sessions_.emplace(stream_name, session).second;
        }
    }
    if (closed || !inserted)
    {
        session->shutdown();
        return closed ? gb28181_create_error::internal_error : gb28181_create_error::duplicate_stream;
    }

    spdlog::info("gb28181 session created {}", stream_name);
    return gb28181_create_error::none;
}

bool gb28181_service::remove(std::string_view stream_name)
{
    std::shared_ptr<gb28181_udp_session> session;
    {
        std::scoped_lock lock(sessions_mutex_);
        const auto iterator = sessions_.find(stream_name);
        if (iterator == sessions_.end())
        {
            return false;
        }
        session = iterator->second.lock();
        if (!session)
        {
            sessions_.erase(iterator);
            return false;
        }
    }
    session->shutdown();
    spdlog::info("gb28181 session removed {}", stream_name);
    return true;
}

void gb28181_service::shutdown()
{
    std::map<std::string, std::weak_ptr<gb28181_udp_session>, std::less<>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        sessions = std::move(sessions_);
        sessions_.clear();
    }
    for (const auto& [name, weak_session] : sessions)
    {
        static_cast<void>(name);
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}

}    // namespace media_server
