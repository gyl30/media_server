#include "media/http/http_server.h"

#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(boost::asio::io_context& io, stream_registry& registry, hls_service& hls, std::uint16_t port)
    : registry_(registry), hls_(hls), listener_(io, port, [this](boost::asio::ip::tcp::socket socket) {
          auto session = std::make_shared<http_session>(std::move(socket), registry_, hls_);
          session->start();
      })
{
}

void http_server::start()
{
    listener_.start();
}

void http_server::close()
{
    listener_.close();
}
}    // namespace media_server
