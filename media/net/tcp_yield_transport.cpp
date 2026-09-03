#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>

#include "media/net/tcp_yield_transport.h"

namespace media_server
{

tcp_yield_transport::tcp_yield_transport(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

std::size_t tcp_yield_transport::read(std::span<std::uint8_t> buffer, boost::asio::yield_context& yield, boost::system::error_code& error)
{
    return socket_.async_read_some(boost::asio::buffer(buffer), yield[error]);
}

std::size_t tcp_yield_transport::write(std::span<const std::uint8_t> data, boost::asio::yield_context& yield, boost::system::error_code& error)
{
    return boost::asio::async_write(socket_, boost::asio::buffer(data), yield[error]);
}

boost::asio::ip::tcp::endpoint tcp_yield_transport::local_endpoint(boost::system::error_code& error) const
{
    return socket_.local_endpoint(error);
}

boost::asio::ip::tcp::endpoint tcp_yield_transport::remote_endpoint(boost::system::error_code& error) const
{
    return socket_.remote_endpoint(error);
}

void tcp_yield_transport::shutdown()
{
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket_.close(error);
}

}    // namespace media_server
