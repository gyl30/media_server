#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <algorithm>

#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/buffer.hpp>

#include "media/net/udp_transport.h"

namespace media_server
{
namespace
{
constexpr std::size_t write_high_water_mark = 1024U * 1024U;
constexpr int media_receive_buffer_bytes = 256 * 1024;
constexpr std::size_t max_held_segments = 64;
constexpr std::size_t max_held_bytes = 63U * 1024U;
}    // namespace

thread_local udp_transport::held_buffer udp_transport::held_buffer_;

udp_transport::udp_transport(boost::asio::io_context& owner) : socket_(owner) {}

void udp_transport::startup(boost::asio::ip::address bind_address,
                            std::uint16_t port,
                            udp_receive_buffer receive_buffer,
                            boost::system::error_code& error)
{
    error.clear();
    if (bind_address.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::udp::endpoint endpoint{bind_address, port};
    socket_.open(endpoint.protocol(), error);
    if (!error && receive_buffer == udp_receive_buffer::media_input)
    {
        socket_.set_option(boost::asio::socket_base::receive_buffer_size(media_receive_buffer_bytes), error);
    }
    if (!error)
    {
        socket_.non_blocking(true, error);
    }
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

void udp_transport::set_error_callback(error_callback callback) { error_callback_ = std::move(callback); }

bool udp_transport::write(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!socket_.is_open())
    {
        return false;
    }
    if (corked_ && write_queue_.empty() && !data.empty())
    {
        if (held_count_ != 0 && !can_hold(data, endpoint))
        {
            send_held();
        }
        if (write_queue_.empty())
        {
            if (held_count_ == 0)
            {
                if (held_buffer_.owner != nullptr)
                {
                    held_buffer_.owner->send_held();
                }
                held_buffer_.owner = this;
                held_buffer_.bytes.clear();
                held_segment_size_ = data.size();
                held_endpoint_ = endpoint;
            }
            held_buffer_.bytes.insert(held_buffer_.bytes.end(), data.begin(), data.end());
            ++held_count_;
            if (data.size() < held_segment_size_ || held_count_ == max_held_segments)
            {
                send_held();
            }
            return true;
        }
    }
    if (write_queue_.empty() && send_now(data, endpoint))
    {
        return true;
    }
    return enqueue(data, endpoint);
}

void udp_transport::cork() { corked_ = true; }

void udp_transport::uncork()
{
    corked_ = false;
    if (held_count_ != 0)
    {
        send_held();
    }
}

bool udp_transport::send_now(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    boost::system::error_code error;
    socket_.send_to(boost::asio::buffer(data.data(), data.size()), endpoint, 0, error);
    if (error == boost::asio::error::would_block)
    {
        return false;
    }
    if (error)
    {
        report_error(error);
    }
    return true;
}

bool udp_transport::enqueue(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (data.size() > write_high_water_mark || queued_write_bytes_ > write_high_water_mark - data.size())
    {
        return false;
    }

    const bool start = write_queue_.empty();
    queued_write_bytes_ += data.size();
    write_queue_.push_back(std::make_shared<pending_datagram>(pending_datagram{{data.begin(), data.end()}, endpoint}));
    if (start)
    {
        start_write();
    }
    return true;
}

bool udp_transport::can_hold(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint) const
{
    return endpoint == held_endpoint_ && data.size() <= held_segment_size_ && held_buffer_.bytes.size() + data.size() <= max_held_bytes;
}

void udp_transport::send_held()
{
    const std::span<const std::uint8_t> held = held_buffer_.bytes;
    const auto count = std::exchange(held_count_, 0);
    held_buffer_.owner = nullptr;
    if (count == 1)
    {
        if (!send_now(held, held_endpoint_))
        {
            enqueue(held, held_endpoint_);
        }
        return;
    }

    iovec vector{const_cast<std::uint8_t*>(held.data()), held.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(std::uint16_t))> control{};
    msghdr message{};
    message.msg_name = held_endpoint_.data();
    message.msg_namelen = static_cast<socklen_t>(held_endpoint_.size());
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_UDP;
    header->cmsg_type = UDP_SEGMENT;
    header->cmsg_len = CMSG_LEN(sizeof(std::uint16_t));
    const auto gso_size = static_cast<std::uint16_t>(held_segment_size_);
    std::memcpy(CMSG_DATA(header), &gso_size, sizeof(gso_size));
    if (::sendmsg(socket_.native_handle(), &message, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0)
    {
        return;
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == EIO || error == EINVAL)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto offset = index * held_segment_size_;
            const auto datagram = held.subspan(offset, std::min(held_segment_size_, held.size() - offset));
            if (error == EAGAIN || error == EWOULDBLOCK || !write_queue_.empty() || !send_now(datagram, held_endpoint_))
            {
                enqueue(datagram, held_endpoint_);
            }
        }
        return;
    }
    report_error(boost::system::error_code{error, boost::system::system_category()});
}

void udp_transport::report_error(boost::system::error_code error)
{
    boost::asio::post(socket_.get_executor(),
                      [self = shared_from_this(), error]()
                      {
                          if (const auto callback = self->error_callback_)
                          {
                              callback(error);
                          }
                      });
}

void udp_transport::start_write()
{
    const auto datagram = write_queue_.front();
    const auto self = shared_from_this();
    socket_.async_send_to(boost::asio::buffer(datagram->packet),
                          datagram->endpoint,
                          [self, datagram](boost::system::error_code error, std::size_t) { self->on_write(datagram, error); });
}

void udp_transport::on_write(const std::shared_ptr<pending_datagram>& datagram, boost::system::error_code error)
{
    if (!socket_.is_open() || write_queue_.empty())
    {
        return;
    }
    if (error)
    {
        if (const auto callback = error_callback_)
        {
            callback(error);
        }
        return;
    }

    queued_write_bytes_ -= datagram->packet.size();
    write_queue_.pop_front();
    if (!write_queue_.empty())
    {
        start_write();
    }
}

void udp_transport::shutdown()
{
    write_queue_.clear();
    queued_write_bytes_ = 0;
    corked_ = false;
    if (held_buffer_.owner == this)
    {
        held_buffer_.owner = nullptr;
        held_buffer_.bytes.clear();
    }
    held_count_ = 0;
    error_callback_ = {};
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
}

}    // namespace media_server
