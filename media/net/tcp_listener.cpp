#include <utility>

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/error.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

tcp_listener::tcp_listener(boost::asio::io_context& io, std::uint16_t port, boost::asio::ip::address bind_address)
    : timer_(io), acceptor_(io), port_(port), bind_address_(std::move(bind_address))
{
}

void tcp_listener::startup(boost::system::error_code& error)
{
    error.clear();
    if (bind_address_.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{bind_address_, port_};
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
    if (error)
    {
        boost::system::error_code close_error;
        acceptor_.close(close_error);
    }
}

void tcp_listener::accept(boost::asio::ip::tcp::socket& socket,
                          std::chrono::milliseconds timeout,
                          boost::asio::yield_context& yield,
                          boost::system::error_code& error)
{
    error.clear();
    if (timeout > std::chrono::milliseconds::zero())
    {
        acceptor_.async_accept(socket, boost::asio::cancel_after(timer_, timeout, yield[error]));
        if (error == boost::asio::error::operation_aborted && !closed_)
        {
            error = boost::asio::error::timed_out;
        }
        return;
    }

    acceptor_.async_accept(socket, yield[error]);
}

void tcp_listener::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

}    // namespace media_server
