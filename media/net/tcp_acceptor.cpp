#include <utility>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include "media/net/tcp_acceptor.h"

namespace media_server
{

tcp_acceptor::tcp_acceptor(boost::asio::any_io_executor executor,
                           std::uint16_t port,
                           boost::asio::ip::address bind_address,
                           std::chrono::milliseconds timeout)
    : acceptor_(executor), timer_(acceptor_.get_executor()), bind_address_(std::move(bind_address)), port_(port), timeout_(timeout)
{
}

boost::system::error_code tcp_acceptor::startup(socket_handler handler)
{
    if (started_ || completed_)
    {
        return boost::asio::error::already_started;
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
    completed_ = false;
    socket_handler_ = std::move(handler);
    if (timeout_ > std::chrono::milliseconds::zero())
    {
        timer_.expires_after(timeout_);
        const auto self = shared_from_this();
        timer_.async_wait(
            [self](const boost::system::error_code& timer_error)
            {
                if (!timer_error)
                {
                    self->complete(boost::asio::error::make_error_code(boost::asio::error::timed_out));
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

                self->complete({}, std::move(socket));
            }));
}

void tcp_acceptor::safe_shutdown()
{
    if (!started_ || completed_)
    {
        return;
    }
    completed_ = true;
    started_ = false;
    socket_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.close(error);
}

void tcp_acceptor::complete(boost::system::error_code error, boost::asio::ip::tcp::socket socket)
{
    if (!started_ || completed_)
    {
        boost::system::error_code ignored;
        socket.close(ignored);
        return;
    }
    completed_ = true;
    started_ = false;
    timer_.cancel();
    boost::system::error_code close_error;
    acceptor_.close(close_error);
    auto handler = std::move(socket_handler_);
    if (error)
    {
        socket.close(close_error);
    }
    if (handler)
    {
        boost::asio::dispatch(acceptor_.get_executor(),
                              [handler = std::move(handler), error, socket = std::move(socket)]() mutable
                              { handler(error, std::move(socket)); });
    }
}

void tcp_acceptor::complete(boost::system::error_code error)
{
    complete(error, boost::asio::ip::tcp::socket{acceptor_.get_executor()});
}

}    // namespace media_server
