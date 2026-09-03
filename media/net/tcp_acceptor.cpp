#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>

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
    if (bind_address_.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
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

    socket_handler_ = std::move(handler);
    const auto self = shared_from_this();
    if (timeout_ > std::chrono::milliseconds::zero())
    {
        timer_.expires_after(timeout_);
        timer_.async_wait([self](const boost::system::error_code& timer_error) { self->on_timeout(timer_error); });
    }
    boost::asio::spawn(
        acceptor_.get_executor(), [self](boost::asio::yield_context yield) { self->run_accept(yield); }, boost::asio::detached);
}

void tcp_acceptor::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_acceptor::run_accept(boost::asio::yield_context yield)
{
    boost::asio::ip::tcp::socket socket{acceptor_.get_executor()};
    boost::system::error_code error;
    acceptor_.async_accept(socket, yield[error]);
    timer_.cancel();

    if (error)
    {
        if (socket_handler_)
        {
            socket_handler_(error, boost::asio::ip::tcp::socket{acceptor_.get_executor()});
        }
        return;
    }

    boost::system::error_code close_error;
    acceptor_.close(close_error);
    socket_handler_({}, std::move(socket));
}

void tcp_acceptor::on_timeout(const boost::system::error_code& error)
{
    if (error)
    {
        return;
    }
    socket_handler_(boost::asio::error::make_error_code(boost::asio::error::timed_out),
                    boost::asio::ip::tcp::socket{acceptor_.get_executor()});
}

void tcp_acceptor::safe_shutdown()
{
    socket_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

}    // namespace media_server
