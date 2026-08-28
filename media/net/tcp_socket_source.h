#ifndef MEDIA_NET_TCP_SOCKET_SOURCE_H
#define MEDIA_NET_TCP_SOCKET_SOURCE_H

#include <functional>

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class tcp_socket_source
{
   public:
    using socket_handler = std::function<void(boost::system::error_code, boost::asio::ip::tcp::socket)>;

    virtual ~tcp_socket_source() = default;

    virtual void startup(socket_handler handler, boost::system::error_code& error) = 0;
    virtual void shutdown() = 0;
};

}    // namespace media_server

#endif
