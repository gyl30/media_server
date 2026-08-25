#include <utility>

#include "media/http/http_server.h"
#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(io_context_pool& workers,
                         const config& config)
    : workers_(workers), config_(config), listener_(std::make_shared<tcp_listener>(workers, config.http_port))
{
}

boost::system::error_code http_server::startup()
{
    const std::weak_ptr<http_server> weak = shared_from_this();
    return listener_->startup(
        [weak](boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(std::move(socket));
            }
        });
}

void http_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    auto session = std::make_shared<http_session>(std::move(socket), workers_, config_);
    session->startup();
}
}    // namespace media_server
