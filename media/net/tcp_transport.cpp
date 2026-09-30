#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "media/net/tcp_transport.h"

namespace media_server
{

tcp_transport::tcp_transport(boost::asio::ip::tcp::socket socket, std::size_t max_write_queue_bytes)
    : socket_(std::move(socket)), max_write_queue_bytes_(max_write_queue_bytes)
{
}

void tcp_transport::set_write_callback(write_callback callback) { write_callback_ = std::move(callback); }

std::size_t tcp_transport::read(std::span<std::uint8_t> buffer, boost::asio::yield_context& yield, boost::system::error_code& error)
{
    return socket_.async_read_some(boost::asio::buffer(buffer), yield[error]);
}

void tcp_transport::write(std::span<const std::uint8_t> data)
{
    if (data.empty())
    {
        return;
    }
    write(std::vector<std::uint8_t>(data.begin(), data.end()));
}

void tcp_transport::write(std::vector<std::uint8_t> data)
{
    if (data.empty())
    {
        return;
    }

    const auto self = shared_from_this();
    boost::asio::post(
        socket_.get_executor(),
        [self, data = std::move(data)]() mutable
        {
            if (self->stopped_)
            {
                return;
            }

            self->queued_write_bytes_ += data.size();
            self->write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(data)));
            if (self->write_queue_.size() == 1)
            {
                self->safe_write();
            }
        });
}

boost::asio::ip::tcp::endpoint tcp_transport::local_endpoint(boost::system::error_code& error) const { return socket_.local_endpoint(error); }

boost::asio::ip::tcp::endpoint tcp_transport::remote_endpoint(boost::system::error_code& error) const { return socket_.remote_endpoint(error); }

void tcp_transport::shutdown()
{
    stopped_ = true;
    write_queue_.clear();
    queued_write_bytes_ = 0;
    write_callback_ = {};

    boost::system::error_code error;
    socket_.cancel(error);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
}

void tcp_transport::safe_write()
{
    if (write_queue_.empty())
    {
        return;
    }

    const auto data = write_queue_.front();
    const auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(*data), [self, data](boost::system::error_code error, std::size_t bytes) { self->on_write(error, bytes); });
}

void tcp_transport::on_write(boost::system::error_code error, std::size_t bytes)
{
    if (stopped_)
    {
        return;
    }

    if (!error)
    {
        queued_write_bytes_ -= write_queue_.front()->size();
        write_queue_.pop_front();
        if (queued_write_bytes_ > max_write_queue_bytes_)
        {
            error = boost::asio::error::no_buffer_space;
        }
    }

    if (error)
    {
        stopped_ = true;
        write_queue_.clear();
        queued_write_bytes_ = 0;
    }

    if (write_callback_)
    {
        write_callback_(error, bytes);
    }
    if (!error)
    {
        safe_write();
    }
}

}    // namespace media_server
