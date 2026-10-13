#include <boost/asio/error.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

tcp_listener::tcp_listener(boost::asio::io_context& io) : acceptor_(io) {}

void tcp_listener::startup(const boost::asio::ip::address& bind_address, std::uint16_t port, boost::system::error_code& error)
{
    error.clear();
    if (bind_address.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{bind_address, port};
    acceptor_.open(endpoint.protocol(), error);
    if (!error)
    {
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
    }
    if (!error)
    {
        acceptor_.bind(endpoint, error);
    }
    if (!error)
    {
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
    }
}

void tcp_listener::accept(boost::asio::ip::tcp::socket& socket, boost::asio::yield_context& yield, boost::system::error_code& error)
{
    error.clear();
    acceptor_.async_accept(socket, yield[error]);
}

void tcp_listener::shutdown()
{
    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

}    // namespace media_server
