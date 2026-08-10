#include "media/rtsp/rtsp_server.h"

#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_output_session.h"

#include <algorithm>

namespace media_server
{

rtsp_server::rtsp_server(boost::asio::io_context& io, stream_registry& registry, std::uint16_t port)
    : registry_(registry),
      port_(port),
      listener_(io,
                port,
                [this](boost::asio::ip::tcp::socket socket)
                {
                    auto connection = std::make_shared<tcp_connection>(std::move(socket));
                    auto session = std::make_shared<rtsp_output_session>(std::move(connection), registry_, port_);
                    std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
                    sessions_.emplace_back(session);
                    session->start();
                })
{
}

boost::system::error_code rtsp_server::start() { return listener_.start(); }

void rtsp_server::close()
{
    listener_.close();
    auto sessions = std::move(sessions_);
    sessions_.clear();
    for (const auto& weak_session : sessions)
    {
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}

}    // namespace media_server
