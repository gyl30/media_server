#include <chrono>
#include <cstring>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/tcp_connection.h"

namespace media_server
{

namespace
{
constexpr auto slow_write_timeout = std::chrono::seconds(15);
}

tcp_connection::tcp_connection(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

void tcp_connection::startup(read_handler on_read, write_handler on_write)
{
    read_handler_ = std::move(on_read);
    write_handler_ = std::move(on_write);

    const auto self = shared_from_this();
    boost::asio::spawn(
        socket_.get_executor(), [self](boost::asio::yield_context yield) { self->run_read(yield); }, boost::asio::detached);
}

void tcp_connection::write(std::span<const std::uint8_t> data)
{
    if (closed_ || data.empty())
    {
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(
            socket_.get_executor(), [self](boost::asio::yield_context yield) { self->run_write(yield); }, boost::asio::detached);
    }
}

void tcp_connection::write(const void* data, std::size_t bytes)
{
    if (!data || bytes == 0)
    {
        return;
    }
    write(std::span{
        static_cast<const std::uint8_t*>(data),
        bytes,
    });
}

void tcp_connection::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self]() { self->safe_shutdown(); });
}

boost::asio::ip::tcp::socket& tcp_connection::socket() noexcept { return socket_; }

void tcp_connection::run_read(boost::asio::yield_context yield)
{
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = socket_.async_read_some(boost::asio::buffer(read_buffer_), yield[error]);
        if (error)
        {
            if (read_handler_)
            {
                read_handler_(error, {});
            }
            return;
        }
        if (bytes != 0 && read_handler_)
        {
            read_handler_(error, std::span{read_buffer_.data(), bytes});
        }
    }
}

void tcp_connection::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (closed_ || write_queue_.empty())
        {
            return;
        }

        const auto batch_size = write_queue_.size();
        std::vector<std::shared_ptr<std::vector<std::uint8_t>>> batch;
        std::vector<boost::asio::const_buffer> buffers;
        batch.reserve(batch_size);
        buffers.reserve(batch_size);
        for (std::size_t index = 0; index < batch_size; ++index)
        {
            batch.push_back(write_queue_[index]);
            buffers.push_back(boost::asio::buffer(*batch.back()));
        }

        boost::system::error_code error;
        const auto started_at = std::chrono::steady_clock::now();
        const auto write_size = boost::asio::async_write(socket_, buffers, yield[error]);
        if (error)
        {
            write_queue_.clear();
            if (write_handler_)
            {
                write_handler_(error, write_size);
            }
            return;
        }

        for (std::size_t index = 0; index < batch_size; ++index)
        {
            write_queue_.pop_front();
        }

        if (std::chrono::steady_clock::now() - started_at > slow_write_timeout)
        {
            write_queue_.clear();
            if (write_handler_)
            {
                write_handler_(boost::asio::error::make_error_code(boost::asio::error::timed_out), write_size);
            }
            return;
        }
        if (write_handler_)
        {
            write_handler_(error, write_size);
        }
        if (write_queue_.empty())
        {
            return;
        }
    }
}
void tcp_connection::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    read_handler_ = {};
    write_handler_ = {};
    write_queue_.clear();

    boost::system::error_code error;
    socket_.cancel(error);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
}

}    // namespace media_server
