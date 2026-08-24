#include <utility>
#include <algorithm>

#include "media/rtmp/rtmp_server.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/tcp_connection.h"

namespace media_server
{

rtmp_server::rtmp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port, output_video_config video)
    : registry_(registry), video_config_(video), listener_(std::make_shared<tcp_listener>(workers, port))
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

void rtmp_server::shutdown()
{
    std::vector<std::weak_ptr<rtmp_session>> sessions;
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

void rtmp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    std::scoped_lock lock(sessions_mutex_);
    if (closed_)
    {
        boost::system::error_code error;
        socket.close(error);
        return;
    }
    auto connection = std::make_shared<tcp_connection>(std::move(socket));
    auto session = std::make_shared<rtmp_session>(std::move(connection), registry_, video_config_);
    std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
    sessions_.emplace_back(session);
    session->startup();
}

}    // namespace media_server
