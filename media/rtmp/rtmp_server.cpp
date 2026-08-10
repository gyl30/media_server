#include "media/rtmp/rtmp_server.h"

#include "media/net/tcp_connection.h"
#include "media/rtmp/rtmp_session.h"

#include <algorithm>
#include <utility>

namespace media_server
{

rtmp_server::rtmp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port)
    : registry_(registry),
      listener_(workers,
                port,
                [this](boost::asio::ip::tcp::socket socket)
                {
                    std::shared_ptr<rtmp_session> session;
                    {
                        std::scoped_lock lock(sessions_mutex_);
                        if (closed_)
                        {
                            boost::system::error_code error;
                            socket.close(error);
                            return;
                        }
                        auto connection = std::make_shared<tcp_connection>(std::move(socket));
                        session = std::make_shared<rtmp_session>(std::move(connection), registry_);
                        std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
                        sessions_.emplace_back(session);
                    }
                    session->start();
                })
{
}

boost::system::error_code rtmp_server::start() { return listener_.start(); }

void rtmp_server::close()
{
    listener_.close();
    std::vector<std::weak_ptr<rtmp_session>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        closed_ = true;
        sessions = std::move(sessions_);
        sessions_.clear();
    }
    for (const auto& weak_session : sessions)
    {
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}

}    // namespace media_server
