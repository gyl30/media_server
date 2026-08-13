#include "media/net/tcp_connection.h"

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstring>
#include <utility>

namespace media_server
{

namespace
{
constexpr auto slow_write_timeout = std::chrono::seconds(15);
}

tcp_connection::tcp_connection(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

void tcp_connection::startup(read_handler on_read, write_error_handler on_write_error)
{
    on_read_ = std::move(on_read);
    on_write_error_ = std::move(on_write_error);
    read_next();
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
        write_next();
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

void tcp_connection::read_next()
{
    if (closed_)
    {
        return;
    }

    const auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
                            [this, self](const boost::system::error_code& error, std::size_t bytes)
                            {
                                if (closed_)
                                {
                                    return;
                                }
                                if (error)
                                {
                                    if (on_read_)
                                    {
                                        on_read_(error, {});
                                    }
                                    return;
                                }
                                if (bytes != 0 && on_read_)
                                {
                                    on_read_(error, std::span{read_buffer_.data(), bytes});
                                }
                                read_next();
                            });
}

void tcp_connection::write_next()
{
    if (closed_ || write_queue_.empty())
    {
        return;
    }

    const auto self = shared_from_this();
    const auto buffer = write_queue_.front();
    const auto started_at = std::chrono::steady_clock::now();
    boost::asio::async_write(socket_,
                             boost::asio::buffer(*buffer),
                             [this, self, buffer, started_at](const boost::system::error_code& error, std::size_t)
                             {
                                 if (closed_)
                                 {
                                     return;
                                 }
                                 if (error)
                                 {
                                     if (on_write_error_)
                                     {
                                         on_write_error_();
                                     }
                                     return;
                                 }
                                 write_queue_.pop_front();
                                 if (std::chrono::steady_clock::now() - started_at > slow_write_timeout)
                                 {
                                     if (on_write_error_)
                                     {
                                         on_write_error_();
                                     }
                                     return;
                                 }
                                 write_next();
                             });
}

void tcp_connection::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    boost::system::error_code error;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
    write_queue_.clear();
    on_read_ = {};
    on_write_error_ = {};
}

}    // namespace media_server
