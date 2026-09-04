#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include "media/net/udp_yield_transport.h"

namespace media_server
{

udp_yield_transport::udp_yield_transport(boost::asio::io_context& owner) : socket_(owner) {}

void udp_yield_transport::startup(boost::asio::ip::address bind_address, std::uint16_t port, boost::system::error_code& error)
{
    error.clear();
    if (socket_.is_open())
    {
        error = boost::asio::error::already_started;
        return;
    }
    if (bind_address.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::udp::endpoint endpoint{bind_address, port};
    socket_.open(endpoint.protocol(), error);
    if (!error)
    {
        socket_.bind(endpoint, error);
    }
    if (error)
    {
        boost::system::error_code close_error;
        socket_.close(close_error);
    }
}

void udp_yield_transport::connect(const boost::asio::ip::udp::endpoint& endpoint, boost::system::error_code& error)
{
    error.clear();
    if (!socket_.is_open())
    {
        error = boost::asio::error::bad_descriptor;
        return;
    }
    socket_.connect(endpoint, error);
}

std::size_t udp_yield_transport::read(std::span<std::uint8_t> buffer,
                                      boost::asio::ip::udp::endpoint& endpoint,
                                      boost::asio::yield_context& yield,
                                      boost::system::error_code& error)
{
    return socket_.async_receive_from(boost::asio::buffer(buffer), endpoint, yield[error]);
}

std::size_t udp_yield_transport::write(std::span<const std::uint8_t> data,
                                       const boost::asio::ip::udp::endpoint& endpoint,
                                       boost::asio::yield_context& yield,
                                       boost::system::error_code& error)
{
    return socket_.async_send_to(boost::asio::buffer(data), endpoint, yield[error]);
}

boost::asio::ip::udp::endpoint udp_yield_transport::local_endpoint(boost::system::error_code& error) const
{
    return socket_.local_endpoint(error);
}

void udp_yield_transport::shutdown()
{
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
}

}    // namespace media_server
