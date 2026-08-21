#include "media/net/tcp_listener.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <utility>

namespace media_server
{

tcp_listener::tcp_listener(io_context_pool& workers, std::uint16_t port)
    : acceptor_(workers.context(0)), workers_(workers), port_(port)
{
}

boost::system::error_code tcp_listener::startup(accept_handler handler, std::size_t accept_limit)
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
    handler_ = std::move(handler);
    accept_limit_ = accept_limit;
    accepted_count_ = 0;
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->accept_next(); });
    return {};
}

void tcp_listener::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_listener::safe_shutdown()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    handler_ = {};
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
    const auto self = shared_from_this();
    acceptor_.async_accept(
        worker,
        boost::asio::bind_executor(
            acceptor_.get_executor(),
            [self](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
            {
                if (!error)
                {
                    ++self->accepted_count_;
                    auto handler = self->handler_;
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
                        boost::asio::dispatch(socket.get_executor(), [handler = std::move(handler), socket = std::move(socket)]() mutable {
                            handler(std::move(socket));
                        });
                    }
                    return;
                }
                if (self->started_)
                {
                    self->accept_next();
                }
            }));
}

}    // namespace media_server
