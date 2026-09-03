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
    const auto self = shared_from_this();
    listener_->startup([self, this](boost::system::error_code ec, worker_context& worker, boost::asio::ip::tcp::socket socket)
                       { on_accept(ec, worker, std::move(socket)); },
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

void rtsp_server::on_accept(boost::system::error_code error, worker_context& worker, boost::asio::ip::tcp::socket socket)
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

    auto tcp = std::make_shared<tcp_connection>(std::move(socket));
    auto connection = std::make_shared<rtsp_server_connection>(worker, std::move(tcp), config_.rtsp_video.codec);
    connection->startup();
}

}    // namespace media_server
