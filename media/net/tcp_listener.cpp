#include "media/net/tcp_listener.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/system/error_code.hpp>

#include <utility>

namespace media_server
{

tcp_listener::tcp_listener(io_context_pool& workers, std::uint16_t port, accept_handler handler)
    : acceptor_(workers.context(0)), workers_(workers), port_(port), handler_(std::move(handler))
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

    auto& worker = workers_.next();
    acceptor_.async_accept(
        worker,
        boost::asio::bind_executor(
            acceptor_.get_executor(),
            [this](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
            {
                if (!error && handler_)
                {
                    auto handler = handler_;
                    boost::asio::dispatch(socket.get_executor(), [handler = std::move(handler), socket = std::move(socket)]() mutable {
                        handler(std::move(socket));
                    });
                }
                if (started_)
                {
                    accept_next();
                }
            }));
}

}    // namespace media_server
