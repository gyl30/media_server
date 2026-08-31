#include <utility>

#include "media/rtsp/rtsp_server.h"
#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers, const config& config)
    : config_(config), listener_(std::make_shared<tcp_listener>(workers, config.rtsp_port, boost::asio::ip::make_address(config.bind_address)))
{
}

void rtsp_server::startup(boost::system::error_code& error)
{
    const std::weak_ptr<rtsp_server> weak = shared_from_this();
    listener_->startup(
        [weak](boost::system::error_code accept_error, boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                if (accept_error)
                {
                    self->shutdown();
                    return;
                }
                self->on_accept(std::move(socket));
            }
        },
        0,
        {},
        error);
}

void rtsp_server::shutdown()
{
    {
        std::scoped_lock lock(mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
    }
    listener_->shutdown();
}

void rtsp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    std::scoped_lock lock(mutex_);
    if (closed_)
    {
        boost::system::error_code error;
        socket.close(error);
        return;
    }

    auto tcp = std::make_shared<tcp_connection>(std::move(socket));
    auto connection = std::make_shared<rtsp_server_connection>(std::move(tcp), config_.rtsp_video.codec);
    connection->startup();
}

}    // namespace media_server
