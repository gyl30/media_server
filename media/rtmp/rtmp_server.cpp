#include "media/rtmp/rtmp_server.h"

#include "media/net/tcp_connection.h"
#include "media/rtmp/rtmp_session.h"

namespace media_server
{

rtmp_server::rtmp_server(
    boost::asio::io_context& io,
    stream_registry& registry,
    std::uint16_t port)
    : registry_(registry),
      listener_(io, port, [this](boost::asio::ip::tcp::socket socket) {
          auto connection = std::make_shared<tcp_connection>(std::move(socket));
          auto session = std::make_shared<rtmp_session>(std::move(connection), registry_);
          session->start();
      })
      {
}

void rtmp_server::start()
{
    listener_.start();
}

void rtmp_server::close()
{
    listener_.close();
}

}    // namespace media_server
