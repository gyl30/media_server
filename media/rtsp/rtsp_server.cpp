#include "media/rtsp/rtsp_server.h"

#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_output_session.h"

#include <algorithm>
#include <utility>

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port)
    : registry_(registry),
      port_(port),
      listener_(workers,
                port,
                [this](boost::asio::ip::tcp::socket socket)
                {
                    std::shared_ptr<rtsp_output_session> session;
                    {
                        std::scoped_lock lock(sessions_mutex_);
                        if (closed_)
                        {
                            boost::system::error_code error;
                            socket.close(error);
                            return;
                        }
                        auto connection = std::make_shared<tcp_connection>(std::move(socket));
                        session = std::make_shared<rtsp_output_session>(std::move(connection), registry_, port_);
                        std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
                        sessions_.emplace_back(session);
                    }
                    session->startup();
                })
{
}

boost::system::error_code rtsp_server::startup() { return listener_.startup(); }

void rtsp_server::shutdown()
{
    listener_.shutdown();
    std::vector<std::weak_ptr<rtsp_output_session>> sessions;
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
