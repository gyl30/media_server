#include <utility>

#include "media/rtmp/rtmp_server.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/tcp_connection.h"

namespace media_server
{

rtmp_server::rtmp_server(io_context_pool& workers, const config& config)
    : config_(config), listener_(std::make_shared<tcp_listener>(workers, config.rtmp_port))
{
}

boost::system::error_code rtmp_server::startup()
{
    const std::weak_ptr<rtmp_server> weak = shared_from_this();
    return listener_->startup(
        [weak](boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(std::move(socket));
            }
        });
}

void rtmp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    auto connection = std::make_shared<tcp_connection>(std::move(socket));
    auto session = std::make_shared<rtmp_session>(std::move(connection), config_.rtmp_video);
    session->startup();
}

}    // namespace media_server
