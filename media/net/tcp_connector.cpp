#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/tcp_connector.h"

namespace media_server
{

tcp_connector::tcp_connector(boost::asio::any_io_executor executor, boost::asio::ip::tcp::endpoint endpoint, std::chrono::milliseconds timeout)
    : socket_(executor), timer_(socket_.get_executor()), endpoint_(std::move(endpoint)), timeout_(timeout)
{
}

void tcp_connector::startup(socket_handler handler, boost::system::error_code& error)
{
    error.clear();
    socket_handler_ = std::move(handler);

    const auto self = shared_from_this();
    if (timeout_ > std::chrono::milliseconds::zero())
    {
        timer_.expires_after(timeout_);
        timer_.async_wait([self](const boost::system::error_code& timer_error) { self->on_timeout(timer_error); });
    }
    boost::asio::spawn(
        socket_.get_executor(), [self](boost::asio::yield_context yield) { self->run_connect(yield); }, boost::asio::detached);
}

void tcp_connector::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_connector::run_connect(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    socket_.async_connect(endpoint_, yield[error]);
    timer_.cancel();

    if (error)
    {
        if (socket_handler_)
        {
            socket_handler_(error, boost::asio::ip::tcp::socket{socket_.get_executor()});
        }
        return;
    }
    socket_handler_({}, std::move(socket_));
}

void tcp_connector::on_timeout(const boost::system::error_code& error)
{
    if (error)
    {
        return;
    }
    socket_handler_(boost::asio::error::make_error_code(boost::asio::error::timed_out),
                    boost::asio::ip::tcp::socket{socket_.get_executor()});
}

void tcp_connector::safe_shutdown()
{
    socket_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
}

}    // namespace media_server
