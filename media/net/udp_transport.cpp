#include <boost/asio/error.hpp>
#include <boost/asio/buffer.hpp>

#include "media/net/udp_transport.h"

namespace media_server
{
namespace
{
constexpr std::size_t write_high_water_mark = 1024U * 1024U;
}    // namespace

udp_transport::udp_transport(boost::asio::io_context& owner) : socket_(owner) {}

void udp_transport::startup(boost::asio::ip::address bind_address, std::uint16_t port, boost::system::error_code& error)
{
    error.clear();
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

void udp_transport::connect(const boost::asio::ip::udp::endpoint& endpoint, boost::system::error_code& error)
{
    error.clear();
    if (!socket_.is_open())
    {
        error = boost::asio::error::bad_descriptor;
        return;
    }
    socket_.connect(endpoint, error);
}

std::size_t udp_transport::read(std::span<std::uint8_t> buffer,
                                      boost::asio::ip::udp::endpoint& endpoint,
                                      boost::asio::yield_context& yield,
                                      boost::system::error_code& error)
{
    return socket_.async_receive_from(boost::asio::buffer(buffer), endpoint, yield[error]);
}

void udp_transport::set_write_callback(write_callback callback)
{
    write_callback_ = callback ? std::make_shared<write_callback>(std::move(callback)) : nullptr;
}

bool udp_transport::write(std::span<const std::uint8_t> data, boost::asio::ip::udp::endpoint endpoint)
{
    return write(std::vector<std::uint8_t>(data.begin(), data.end()), std::move(endpoint));
}

bool udp_transport::write(std::vector<std::uint8_t> data, boost::asio::ip::udp::endpoint endpoint)
{
    if (!socket_.is_open())
    {
        return false;
    }
    if (data.size() > write_high_water_mark || queued_write_bytes_ > write_high_water_mark - data.size())
    {
        return false;
    }

    const bool start = write_queue_.empty();
    queued_write_bytes_ += data.size();
    write_queue_.push_back(std::make_shared<pending_datagram>(pending_datagram{std::move(data), std::move(endpoint)}));
    if (start)
    {
        start_write();
    }
    return true;
}

void udp_transport::start_write()
{
    const auto datagram = write_queue_.front();
    const auto self = shared_from_this();
    socket_.async_send_to(boost::asio::buffer(datagram->packet), datagram->endpoint,
                          [self, datagram](boost::system::error_code error, std::size_t bytes) { self->on_write(datagram, error, bytes); });
}

void udp_transport::on_write(const std::shared_ptr<pending_datagram>& datagram, boost::system::error_code error, std::size_t bytes)
{
    if (!socket_.is_open() || write_queue_.empty())
    {
        return;
    }

    const auto callback = write_callback_;
    if (callback)
    {
        (*callback)(error, bytes);
    }
    if (error || !socket_.is_open() || write_queue_.empty())
    {
        return;
    }

    queued_write_bytes_ -= datagram->packet.size();
    write_queue_.pop_front();
    if (!write_queue_.empty())
    {
        start_write();
    }
}

boost::asio::ip::udp::endpoint udp_transport::local_endpoint(boost::system::error_code& error) const { return socket_.local_endpoint(error); }

void udp_transport::shutdown()
{
    write_queue_.clear();
    queued_write_bytes_ = 0;
    write_callback_ = {};
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
}

}    // namespace media_server
