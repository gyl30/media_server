#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

tcp_listener::tcp_listener(io_context_pool& workers, std::uint16_t port, boost::asio::ip::address bind_address)
    : worker_(workers.next()), timer_(worker_.io()), acceptor_(worker_.io()), port_(port), bind_address_(std::move(bind_address)), workers_(workers)
{
}

void tcp_listener::startup(accept_handler handler, std::chrono::milliseconds timeout, boost::system::error_code& ec)
{
    if (bind_address_.is_unspecified())
    {
        ec = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{
        bind_address_,
        port_,
    };
    acceptor_.open(endpoint.protocol(), ec);
    if (!ec)
    {
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    }
    if (!ec)
    {
        acceptor_.bind(endpoint, ec);
    }
    if (!ec)
    {
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    }
    if (ec)
    {
        return;
    }

    accept_handler_ = std::move(handler);
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self, timeout](boost::asio::yield_context yield) { self->accept_loop(timeout, yield); }, boost::asio::detached);
}

void tcp_listener::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [this, self]() { safe_shutdown(); });
}

void tcp_listener::accept_loop(std::chrono::milliseconds timeout, boost::asio::yield_context& yield)
{
    boost::system::error_code ec;
    for (;;)
    {
        if (timeout > std::chrono::milliseconds::zero())
        {
            timer_.expires_after(timeout);
            const auto self = shared_from_this();
            timer_.async_wait([self, this](const boost::system::error_code& timer_error) { on_timeout(timer_error); });
        }

        auto* worker = &workers_.next();
        boost::asio::ip::tcp::socket socket{worker->io()};
        acceptor_.async_accept(socket, yield[ec]);
        timer_.cancel();
        if (ec)
        {
            if (accept_handler_)
            {
                accept_handler_(ec, *worker, std::move(socket));
            }
            break;
        }

        accept_handler_({}, *worker, std::move(socket));
    }
}

void tcp_listener::on_timeout(const boost::system::error_code& ec)
{
    if (ec)
    {
        return;
    }

    accept_handler_(boost::asio::error::make_error_code(boost::asio::error::timed_out), worker_, boost::asio::ip::tcp::socket{worker_.io()});
}

void tcp_listener::safe_shutdown()
{
    accept_handler_ = {};
    timer_.cancel();
    boost::system::error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
}

}    // namespace media_server
