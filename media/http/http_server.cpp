#include <utility>
#include <algorithm>

#include "media/http/http_server.h"
#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(io_context_pool& workers,
                         const config& config,
                         whep_service& whep,
                         gb28181_service& gb28181)
    : config_(config), whep_(whep), gb28181_(gb28181), listener_(std::make_shared<tcp_listener>(workers, config.http_port))
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

void http_server::shutdown()
{
    std::vector<std::weak_ptr<http_session>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        sessions = std::move(sessions_);
        sessions_.clear();
    }
    listener_->shutdown();
    for (const auto& weak_session : sessions)
    {
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}

void http_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    std::scoped_lock lock(sessions_mutex_);
    if (closed_)
    {
        boost::system::error_code error;
        socket.close(error);
        return;
    }
    auto session = std::make_shared<http_session>(std::move(socket), config_, whep_, gb28181_);
    std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
    sessions_.emplace_back(session);
    session->startup();
}
}    // namespace media_server
