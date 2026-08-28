#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/bind_executor.hpp>

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

void tcp_acceptor::startup(socket_handler handler, boost::system::error_code& error)
{
    error.clear();
    if (started_ || completed_)
    {
        error = boost::asio::error::already_started;
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
        return;
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
}

void tcp_acceptor::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_acceptor::accept_next()
{
    if (!started_ || completed_)
    {
        return;
    }

    const auto self = shared_from_this();
    acceptor_.async_accept(boost::asio::bind_executor(acceptor_.get_executor(),
                                                      [self](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
                                                      {
                                                          if (!self->started_ || self->completed_)
                                                          {
                                                              boost::system::error_code ignored;
                                                              socket.close(ignored);
                                                              return;
                                                          }
                                                          self->complete(error, std::move(socket));
                                                      }));
}

void tcp_acceptor::safe_shutdown()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    completed_ = true;
    socket_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.cancel(error);
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
    timer_.cancel();
    auto handler = std::move(socket_handler_);

    if (error)
    {
        boost::system::error_code ignored;
        acceptor_.cancel(ignored);
        socket.close(ignored);
        if (handler)
        {
            boost::asio::dispatch(acceptor_.get_executor(),
                                  [handler = std::move(handler), error, executor = acceptor_.get_executor()]() mutable
                                  { handler(error, boost::asio::ip::tcp::socket{executor}); });
        }
        return;
    }

    started_ = false;
    boost::system::error_code close_error;
    acceptor_.close(close_error);
    if (handler)
    {
        boost::asio::dispatch(acceptor_.get_executor(),
                              [handler = std::move(handler), socket = std::move(socket)]() mutable { handler({}, std::move(socket)); });
    }
}

void tcp_acceptor::complete(boost::system::error_code error) { complete(error, boost::asio::ip::tcp::socket{acceptor_.get_executor()}); }

}    // namespace media_server
