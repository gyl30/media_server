#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/buffer.hpp>

#include "media/net/udp_socket.h"

namespace media_server
{

udp_socket::udp_socket(boost::asio::any_io_executor executor) : socket_(std::move(executor)) {}

bool udp_socket::startup(boost::asio::ip::address bind_address, read_handler on_read, write_error_handler on_write_error)
{
    return startup(std::move(bind_address), 0, std::move(on_read), std::move(on_write_error));
}

bool udp_socket::startup(boost::asio::ip::address bind_address, std::uint16_t port, read_handler on_read, write_error_handler on_write_error)
{
    if (closed_ || socket_.is_open())
    {
        return false;
    }

    boost::system::error_code error;
    socket_.open(bind_address.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4(), error);
    if (error)
    {
        return false;
    }

    socket_.bind(boost::asio::ip::udp::endpoint(std::move(bind_address), port), error);
    if (error)
    {
        socket_.close(error);
        return false;
    }

    const auto endpoint = socket_.local_endpoint(error);
    if (error || endpoint.port() == 0)
    {
        socket_.close(error);
        return false;
    }

    on_read_ = std::move(on_read);
    on_write_error_ = std::move(on_write_error);
    local_port_ = endpoint.port();
    receive_next();
    return true;
}

bool udp_socket::connect(const boost::asio::ip::udp::endpoint& endpoint)
{
    if (closed_ || !socket_.is_open())
    {
        return false;
    }

    boost::system::error_code error;
    socket_.connect(endpoint, error);
    return !error;
}

void udp_socket::send(std::vector<std::uint8_t> packet, boost::asio::ip::udp::endpoint endpoint)
{
    if (closed_ || !socket_.is_open() || packet.empty())
    {
        return;
    }

    const bool start_write = send_queue_.empty();
    send_queue_.push_back(pending_datagram{
        .packet = std::make_shared<std::vector<std::uint8_t>>(std::move(packet)),
        .endpoint = std::move(endpoint),
    });
    if (start_write)
    {
        write_next();
    }
}

void udp_socket::shutdown() { shutdown(shutdown_handler{}); }

void udp_socket::shutdown(shutdown_handler handler)
{
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self, handler = std::move(handler)]() mutable { self->safe_shutdown(std::move(handler)); });
}

std::uint16_t udp_socket::local_port() const noexcept { return local_port_; }

void udp_socket::receive_next()
{
    if (closed_ || !socket_.is_open())
    {
        return;
    }

    const auto self = shared_from_this();
    socket_.async_receive_from(boost::asio::buffer(receive_buffer_),
                               receive_endpoint_,
                               [this, self](boost::system::error_code error, std::size_t bytes)
                               {
                                   if (closed_)
                                   {
                                       return;
                                   }
                                   if (error)
                                   {
                                       if (on_read_)
                                       {
                                           on_read_(error, {}, receive_endpoint_);
                                       }
                                       return;
                                   }
                                   if (on_read_)
                                   {
                                       on_read_(error, std::span{receive_buffer_.data(), bytes}, receive_endpoint_);
                                   }
                                   receive_next();
                               });
}

void udp_socket::write_next()
{
    if (closed_ || !socket_.is_open() || send_queue_.empty())
    {
        return;
    }

    const auto datagram = send_queue_.front();
    const auto self = shared_from_this();
    socket_.async_send_to(boost::asio::buffer(*datagram.packet),
                          datagram.endpoint,
                          [this, self, datagram](boost::system::error_code error, std::size_t)
                          {
                              if (closed_)
                              {
                                  return;
                              }
                              send_queue_.pop_front();
                              write_next();
                              if (error && on_write_error_)
                              {
                                  on_write_error_(error, datagram.endpoint);
                              }
                          });
}

void udp_socket::safe_shutdown(shutdown_handler handler)
{
    if (closed_)
    {
        if (handler)
        {
            handler();
        }
        return;
    }
    closed_ = true;

    on_read_ = {};
    on_write_error_ = {};
    send_queue_.clear();
    local_port_ = 0;
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
    if (handler)
    {
        handler();
    }
}

}    // namespace media_server
