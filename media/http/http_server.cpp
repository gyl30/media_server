#include "media/http/http_server.h"

#include "media/http/http_session.h"

#include <algorithm>
#include <utility>

namespace media_server
{
http_server::http_server(io_context_pool& workers,
                         stream_registry& registry,
                         hls_service& hls,
                         whep_service& whep,
                         std::uint16_t port,
                         output_video_config video)
    : registry_(registry), hls_(hls), whep_(whep), video_config_(video), listener_(std::make_shared<tcp_listener>(workers, port))
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
    auto session = std::make_shared<http_session>(std::move(socket), registry_, hls_, whep_, video_config_);
    std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
    sessions_.emplace_back(session);
    session->startup();
}
}    // namespace media_server
