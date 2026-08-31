#include <utility>

#include "media/http/http_server.h"
#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(io_context_pool& workers, const config& config)
    : workers_(workers),
      config_(config),
      listener_(std::make_shared<tcp_listener>(workers, config.http_port, boost::asio::ip::make_address(config.bind_address)))
{
}

void http_server::startup(boost::system::error_code& error)
{
    const std::weak_ptr<http_server> weak = shared_from_this();
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

void http_server::shutdown()
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

void http_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    std::scoped_lock lock(mutex_);
    if (closed_)
    {
        boost::system::error_code error;
        socket.close(error);
        return;
    }

    auto session = std::make_shared<http_session>(std::move(socket), workers_, config_);
    session->startup();
}
}    // namespace media_server
