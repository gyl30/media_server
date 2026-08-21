#include "media/net/tcp_connector.h"

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include <utility>

namespace media_server
{

tcp_connector::tcp_connector(boost::asio::any_io_executor executor) : socket_(executor), timer_(std::move(executor)) {}

void tcp_connector::startup(boost::asio::ip::tcp::endpoint endpoint, std::chrono::milliseconds timeout, connect_handler handler)
{
    handler_ = std::move(handler);

    timer_.expires_after(timeout);
    const auto self = shared_from_this();
    timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (!error)
            {
                self->complete(boost::asio::error::make_error_code(boost::asio::error::timed_out));
            }
        });
    socket_.async_connect(std::move(endpoint), [self](const boost::system::error_code& error) { self->complete(error); });
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

    auto handler = std::move(handler_);
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

    handler_ = {};
    timer_.cancel();
    boost::system::error_code ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
}

}    // namespace media_server
