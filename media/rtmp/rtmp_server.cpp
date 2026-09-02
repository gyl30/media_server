#include <utility>

#include "media/rtmp/rtmp_server.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/tcp_connection.h"

namespace media_server
{

rtmp_server::rtmp_server(io_context_pool& workers, const config& config)
    : config_(config), listener_(std::make_shared<tcp_listener>(workers, config.rtmp_port, boost::asio::ip::make_address(config.bind_address)))
{
}

void rtmp_server::startup(boost::system::error_code& error)
{
    const std::weak_ptr<rtmp_server> weak = shared_from_this();
    listener_->startup(
        [weak](boost::system::error_code accept_error, worker_context& worker, boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(accept_error, worker, std::move(socket));
            }
        },
        0,
        {},
        error);
}

void rtmp_server::shutdown()
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

void rtmp_server::on_accept(boost::system::error_code error, worker_context& worker, boost::asio::ip::tcp::socket socket)
{
    if (error)
    {
        shutdown();
        return;
    }

    std::scoped_lock lock(mutex_);
    if (closed_)
    {
        boost::system::error_code close_error;
        socket.close(close_error);
        return;
    }

    auto connection = std::make_shared<tcp_connection>(std::move(socket));
    auto session = std::make_shared<rtmp_session>(worker, std::move(connection), config_.rtmp_video);
    session->startup();
}

}    // namespace media_server
