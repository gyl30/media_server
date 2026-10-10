#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "media/net/tcp_transport.h"

namespace media_server
{
namespace
{
constexpr std::size_t write_high_water_mark = 1024U * 1024U;
}    // namespace

tcp_transport::tcp_transport(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

void tcp_transport::set_write_callback(write_callback callback) { write_callback_ = std::move(callback); }

std::size_t tcp_transport::read(std::span<std::uint8_t> data, boost::asio::yield_context& yield, boost::system::error_code& error)
{
    return socket_.async_read_some(boost::asio::buffer(data), yield[error]);
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
    boost::asio::post(socket_.get_executor(), [self, data = std::move(data)]() mutable { self->safe_write(std::move(data)); });
}

boost::asio::ip::tcp::endpoint tcp_transport::local_endpoint(boost::system::error_code& error) const { return socket_.local_endpoint(error); }

boost::asio::ip::tcp::endpoint tcp_transport::remote_endpoint(boost::system::error_code& error) const { return socket_.remote_endpoint(error); }

void tcp_transport::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self]() { self->safe_shutdown(); });
}

void tcp_transport::safe_write(std::vector<std::uint8_t> data)
{
    if (!socket_.is_open() || !write_callback_)
    {
        return;
    }

    const bool writing = !write_queue_.empty();
    // 对端停止读取时在途写不会完成，因此入队时限制尚未开始发送的字节数。
    if (writing && queued_write_bytes_ - write_queue_.front()->size() + data.size() > write_high_water_mark)
    {
        const auto callback = std::exchange(write_callback_, {});
        if (callback)
        {
            callback(boost::asio::error::no_buffer_space, 0);
        }
        return;
    }
    queued_write_bytes_ += data.size();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(data)));
    if (!writing)
    {
        post_write();
    }
}

void tcp_transport::post_write()
{
    const auto data = write_queue_.front();
    const auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(*data), [self, data](boost::system::error_code error, std::size_t bytes) { self->on_write(error, bytes); });
}

void tcp_transport::on_write(boost::system::error_code error, std::size_t bytes)
{
    if (!socket_.is_open() || write_queue_.empty() || !write_callback_)
    {
        return;
    }

    if (!error)
    {
        queued_write_bytes_ -= write_queue_.front()->size();
        write_queue_.pop_front();
    }

    if (write_callback_)
    {
        write_callback_(error, bytes);
    }
    if (error)
    {
        return;
    }
    if (!write_queue_.empty() && write_callback_)
    {
        post_write();
    }
}

void tcp_transport::safe_shutdown()
{
    write_queue_.clear();
    queued_write_bytes_ = 0;
    write_callback_ = {};

    boost::system::error_code error;
    socket_.cancel(error);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
}

}    // namespace media_server
