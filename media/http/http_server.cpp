#include "media/http/http_server.h"

#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(boost::asio::io_context& io, stream_registry& registry, hls_service& hls, whep_service& whep, std::uint16_t port)
    : registry_(registry),
      hls_(hls),
      whep_(whep),
      listener_(io,
                port,
                [this](boost::asio::ip::tcp::socket socket)
                {
                    auto session = std::make_shared<http_session>(std::move(socket), registry_, hls_, whep_);
                    session->start();
                })
{
}

boost::system::error_code http_server::start() { return listener_.start(); }

void http_server::close() { listener_.close(); }
}    // namespace media_server
