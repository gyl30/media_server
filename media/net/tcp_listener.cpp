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

void tcp_listener::startup(accept_handler handler, std::size_t accept_limit, std::chrono::milliseconds timeout, boost::system::error_code& error)
{
    error.clear();
    if (started_)
    {
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{
        bind_address_,
        port_,
    };
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
    accepting_ = true;
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
}

void tcp_listener::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_listener::schedule_timeout()
{
    if (!started_ || !accepting_ || timeout_ <= std::chrono::milliseconds::zero())
    {
        return;
    }

    timer_.expires_after(timeout_);
    const auto self = shared_from_this();
    timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || !self->started_ || !self->accepting_)
            {
                return;
            }

            self->accepting_ = false;
            boost::system::error_code ignored;
            self->acceptor_.cancel(ignored);
            auto handler = std::move(self->accept_handler_);
            if (handler)
            {
                handler(boost::asio::error::make_error_code(boost::asio::error::timed_out),
                        boost::asio::ip::tcp::socket{self->acceptor_.get_executor()});
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
    accepting_ = false;
    accept_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

void tcp_listener::accept_next()
{
    if (!started_ || !accepting_)
    {
        return;
    }

    const auto self = shared_from_this();
    auto on_accept = [self](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
    {
        if (!self->started_ || !self->accepting_)
        {
            boost::system::error_code ignored;
            socket.close(ignored);
            return;
        }
        if (error)
        {
            self->accepting_ = false;
            self->timer_.cancel();
            auto handler = std::move(self->accept_handler_);
            if (handler)
            {
                boost::asio::dispatch(self->acceptor_.get_executor(),
                                      [self, handler = std::move(handler), error]() mutable
                                      { handler(error, boost::asio::ip::tcp::socket{self->acceptor_.get_executor()}); });
            }
            return;
        }

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
                                  [self, handler = std::move(handler), socket = std::move(socket)]() mutable { handler({}, std::move(socket)); });
        }
    };

    auto& worker = workers_.next();
    acceptor_.async_accept(worker, boost::asio::bind_executor(acceptor_.get_executor(), std::move(on_accept)));
}

}    // namespace media_server
