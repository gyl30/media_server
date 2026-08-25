#include <utility>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include "media/net/tcp_acceptor.h"

namespace media_server
{

tcp_acceptor::tcp_acceptor(boost::asio::any_io_executor executor, std::uint16_t port, boost::asio::ip::address bind_address)
    : acceptor_(executor), timer_(acceptor_.get_executor()), bind_address_(std::move(bind_address)), port_(port)
{
}

boost::system::error_code tcp_acceptor::startup(accept_handler handler, std::chrono::milliseconds timeout)
{
    if (started_)
    {
        return {};
    }

    const boost::asio::ip::tcp::endpoint endpoint{bind_address_, port_};
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
    handler_ = std::move(handler);
    if (timeout > std::chrono::milliseconds::zero())
    {
        timer_.expires_after(timeout);
        const auto self = shared_from_this();
        timer_.async_wait(
            [self](const boost::system::error_code& timer_error)
            {
                if (!timer_error)
                {
                    self->safe_shutdown();
                }
            });
    }
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->accept_next(); });
    return {};
}

void tcp_acceptor::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_acceptor::accept_next()
{
    if (!started_)
    {
        return;
    }

    const auto self = shared_from_this();
    acceptor_.async_accept(
        boost::asio::bind_executor(
            acceptor_.get_executor(),
            [self](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
            {
                if (!self->started_)
                {
                    return;
                }
                if (error)
                {
                    self->accept_next();
                    return;
                }

                auto handler = std::move(self->handler_);
                self->safe_shutdown();
                if (handler)
                {
                    boost::asio::dispatch(socket.get_executor(),
                                          [handler = std::move(handler), socket = std::move(socket)]() mutable { handler(std::move(socket)); });
                }
            }));
}

void tcp_acceptor::safe_shutdown()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.close(error);
}

}    // namespace media_server
