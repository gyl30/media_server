#include "media/net/tcp_listener.h"

#include <boost/system/error_code.hpp>
#include <utility>

namespace media_server
{

tcp_listener::tcp_listener(
    boost::asio::io_context& io,
    std::uint16_t port,
    accept_handler handler)
    : acceptor_(io), handler_(std::move(handler))
{

    boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::tcp::v4(),
        port,
    };
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

void tcp_listener::start()
{
    if (started_)
    {
        return;
    }
    started_ = true;
    accept_next();
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
        [this](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket) {
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
