#include <algorithm>
#include <utility>
#include <vector>

#include "media/rtsp/rtsp_server.h"
#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers, const config& config)
    : config_(config), listener_(std::make_shared<tcp_listener>(workers, config.rtsp_port))
{
}

boost::system::error_code rtsp_server::startup()
{
    const std::weak_ptr<rtsp_server> weak = shared_from_this();
    return listener_->startup(
        [weak](boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(std::move(socket));
            }
        });
}

void rtsp_server::shutdown()
{
    std::vector<std::weak_ptr<rtsp_server_connection>> connections;
    {
        std::scoped_lock lock(connections_mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        connections = std::move(connections_);
        connections_.clear();
    }

    listener_->shutdown();
    for (const auto& weak_connection : connections)
    {
        if (const auto connection = weak_connection.lock())
        {
            connection->shutdown();
        }
    }
}

void rtsp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    auto tcp = std::make_shared<tcp_connection>(std::move(socket));
    auto connection = std::make_shared<rtsp_server_connection>(std::move(tcp), config_);
    {
        std::scoped_lock lock(connections_mutex_);
        if (closed_)
        {
            connection->shutdown();
            return;
        }
        std::erase_if(connections_, [](const auto& value) { return value.expired(); });
        connections_.emplace_back(connection);
    }
    connection->startup();
}

}    // namespace media_server
