#include "media/http/http_server.h"

#include "media/http/http_session.h"

#include <algorithm>
#include <utility>

namespace media_server
{
http_server::http_server(io_context_pool& workers, stream_registry& registry, hls_service& hls, whep_service& whep, std::uint16_t port)
    : registry_(registry),
      hls_(hls),
      whep_(whep),
      listener_(workers,
                port,
                [this](boost::asio::ip::tcp::socket socket)
                {
                    std::shared_ptr<http_session> session;
                    {
                        std::scoped_lock lock(sessions_mutex_);
                        if (closed_)
                        {
                            boost::system::error_code error;
                            socket.close(error);
                            return;
                        }
                        session = std::make_shared<http_session>(std::move(socket), registry_, hls_, whep_);
                        std::erase_if(sessions_, [](const auto& value) { return value.expired(); });
                        sessions_.emplace_back(session);
                    }
                    session->startup();
                })
{
}

boost::system::error_code http_server::startup() { return listener_.startup(); }

void http_server::shutdown()
{
    listener_.shutdown();
    std::vector<std::weak_ptr<http_session>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        closed_ = true;
        sessions = std::move(sessions_);
        sessions_.clear();
    }
    for (const auto& weak_session : sessions)
    {
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}
}    // namespace media_server
