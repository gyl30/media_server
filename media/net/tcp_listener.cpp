#include "media/net/tcp_listener.h"

#include <boost/system/error_code.hpp>
#include <utility>

namespace media_server
{

tcp_listener::tcp_listener(boost::asio::io_context& io, std::uint16_t port, accept_handler handler)
    : acceptor_(io), port_(port), handler_(std::move(handler))
{
}

boost::system::error_code tcp_listener::start()
{
    if (started_)
    {
        return {};
    }

    const boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::tcp::v4(),
        port_,
    };
    boost::system::error_code error;
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
        return error;
    }

    started_ = true;
    accept_next();
    return {};
}

void tcp_listener::close()
{
    started_ = false;
    boost::system::error_code error;
    acceptor_.close(error);
}

void tcp_listener::accept_next()
{
    if (!started_)
    {
        return;
    }

    acceptor_.async_accept(
        [this](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
        {
            if (!error && handler_)
            {
                handler_(std::move(socket));
            }
            if (started_)
            {
                accept_next();
            }
        });
}

}    // namespace media_server
