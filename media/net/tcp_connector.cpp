#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>

#include "media/net/tcp_connector.h"

namespace media_server
{

tcp_connector::tcp_connector(boost::asio::any_io_executor executor,
                             boost::asio::ip::tcp::endpoint endpoint,
                             std::chrono::milliseconds timeout)
    : socket_(executor), timer_(socket_.get_executor()), endpoint_(std::move(endpoint)), timeout_(timeout)
{
}

boost::system::error_code tcp_connector::startup(socket_handler handler)
{
    if (started_)
    {
        return boost::asio::error::already_started;
    }

    started_ = true;
    socket_handler_ = std::move(handler);

    const auto self = shared_from_this();
    if (timeout_ > std::chrono::milliseconds::zero())
    {
        timer_.expires_after(timeout_);
        timer_.async_wait(
            [self](const boost::system::error_code& error)
            {
                if (!error)
                {
                    self->complete(boost::asio::error::make_error_code(boost::asio::error::timed_out));
                }
            });
    }
    socket_.async_connect(endpoint_, [self](const boost::system::error_code& error) { self->complete(error); });
    return {};
}

void tcp_connector::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_connector::complete(boost::system::error_code error)
{
    if (completed_)
    {
        return;
    }
    completed_ = true;

    timer_.cancel();
    boost::system::error_code ignored;
    if (error)
    {
        socket_.cancel(ignored);
        socket_.close(ignored);
    }

    auto handler = std::move(socket_handler_);
    auto socket = std::move(socket_);
    if (handler)
    {
        handler(error, std::move(socket));
    }
}

void tcp_connector::safe_shutdown()
{
    if (completed_)
    {
        return;
    }
    completed_ = true;

    socket_handler_ = {};
    timer_.cancel();
    boost::system::error_code ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
}

}    // namespace media_server
