#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/bind_executor.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

tcp_listener::tcp_listener(io_context_pool& workers, std::uint16_t port, boost::asio::ip::address bind_address)
    : acceptor_(workers.context(0)), timer_(workers.context(0)), workers_(workers), bind_address_(std::move(bind_address)), port_(port)
{
}

boost::system::error_code tcp_listener::startup(accept_handler handler, std::size_t accept_limit, std::chrono::milliseconds timeout)
{
    if (started_)
    {
        return {};
    }

    const boost::asio::ip::tcp::endpoint endpoint{
        bind_address_,
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
    accept_handler_ = std::move(handler);
    timeout_ = timeout;
    accept_limit_ = accept_limit;
    accepted_count_ = 0;
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(),
                      [self]()
                      {
                          self->accept_next();
                          self->schedule_timeout();
                      });
    return {};
}

void tcp_listener::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_listener::schedule_timeout()
{
    if (!started_ || timeout_ <= std::chrono::milliseconds::zero())
    {
        return;
    }

    timer_.expires_after(timeout_);
    const auto self = shared_from_this();
    timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (!error)
            {
                self->safe_shutdown();
            }
        });
}

void tcp_listener::safe_shutdown()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    accept_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.close(error);
}

void tcp_listener::accept_next()
{
    if (!started_)
    {
        return;
    }

    const auto self = shared_from_this();
    auto on_accept = [self](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
    {
        if (!error)
        {
            ++self->accepted_count_;
            auto handler = self->accept_handler_;
            if (self->accept_limit_ != 0 && self->accepted_count_ >= self->accept_limit_)
            {
                self->safe_shutdown();
            }
            else
            {
                self->accept_next();
            }
            if (handler)
            {
                boost::asio::dispatch(socket.get_executor(),
                                      [self, handler = std::move(handler), socket = std::move(socket)]() mutable { handler(std::move(socket)); });
            }
            return;
        }
        if (self->started_)
        {
            self->accept_next();
        }
    };

    auto& worker = workers_.next();
    acceptor_.async_accept(worker, boost::asio::bind_executor(acceptor_.get_executor(), std::move(on_accept)));
}

}    // namespace media_server
