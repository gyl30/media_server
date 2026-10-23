#include <random>
#include <cstring>
#include <utility>

#include <boost/json.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include "bench/clients/gb_client.h"

namespace media_server::bench
{

namespace
{

constexpr std::uint64_t video_clock_khz = 90;
constexpr std::size_t receive_buffer_bytes = 2U + 65535U;
constexpr int udp_receive_buffer_bytes = 4 * 1024 * 1024;
constexpr std::uint8_t pack_start = 0xBA;
constexpr std::uint8_t system_header = 0xBB;
constexpr std::uint8_t stream_map = 0xBC;

gb_client_options& options()
{
    static gb_client_options value;
    return value;
}

std::vector<gb_sender_key>& senders()
{
    static std::vector<gb_sender_key> value;
    return value;
}

std::uint16_t next_listen_port()
{
    static std::uint16_t offset{};
    return static_cast<std::uint16_t>(options().listen_port + offset++);
}

std::uint32_t next_ssrc()
{
    static std::uint32_t value = (std::random_device{}() & 0x7FFFF000U) | 1U;
    return value++;
}

std::string_view transport_name(gb_transport transport)
{
    switch (transport)
    {
        case gb_transport::udp:
            return "udp";
        case gb_transport::tcp_active:
            return "tcp_active";
        case gb_transport::tcp_passive:
            return "tcp_passive";
    }
    return {};
}

bool start_code(std::span<const std::uint8_t> data, std::size_t offset)
{
    return offset + 4U <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1;
}

struct ps_frame
{
    bool video{};
    std::optional<std::uint32_t> pts;
};

std::optional<ps_frame> parse_ps_frame(std::span<const std::uint8_t> data)
{
    if (data.size() < 14U || !start_code(data, 0) || data[3] != pack_start || (data[4] & 0xC0U) != 0x40U)
    {
        return std::nullopt;
    }
    std::size_t offset = 14U + (data[13] & 7U);
    while (start_code(data, offset) && offset + 6U <= data.size())
    {
        const auto id = data[offset + 3];
        const auto length = static_cast<std::size_t>((data[offset + 4] << 8U) | data[offset + 5]);
        if (id == system_header || id == stream_map)
        {
            offset += 6U + length;
            continue;
        }
        const bool video = (id & 0xF0U) == 0xE0U;
        if (!video && (id & 0xE0U) != 0xC0U)
        {
            return std::nullopt;
        }
        if (offset + 9U > data.size() || (data[offset + 6] & 0xC0U) != 0x80U)
        {
            return std::nullopt;
        }
        if ((data[offset + 7] & 0x80U) == 0)
        {
            return ps_frame{.video = video, .pts = std::nullopt};
        }
        const auto* stamp = data.data() + offset + 9;
        if (data[offset + 8] < 5U || offset + 14U > data.size() || (stamp[0] & 1U) == 0 || (stamp[2] & 1U) == 0 || (stamp[4] & 1U) == 0)
        {
            return std::nullopt;
        }
        const auto pts = (static_cast<std::uint64_t>((stamp[0] >> 1U) & 7U) << 30U) | (static_cast<std::uint64_t>(stamp[1]) << 22U) |
                         (static_cast<std::uint64_t>(stamp[2] >> 1U) << 15U) | (static_cast<std::uint64_t>(stamp[3]) << 7U) |
                         (static_cast<std::uint64_t>(stamp[4]) >> 1U);
        return ps_frame{.video = video, .pts = static_cast<std::uint32_t>(pts)};
    }
    return std::nullopt;
}

std::uint32_t read_u32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) | (static_cast<std::uint32_t>(data[2]) << 8U) |
           data[3];
}

boost::asio::awaitable<unsigned> control_request(std::string host, std::uint16_t port, std::string target, boost::json::object body)
{
    namespace http = boost::beast::http;
    boost::system::error_code error;
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::ip::tcp::resolver resolver(executor);
    const auto endpoints =
        co_await resolver.async_resolve(host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return 0;
    }
    boost::beast::tcp_stream stream(executor);
    stream.expires_after(std::chrono::seconds{10});
    co_await stream.async_connect(endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return 0;
    }
    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, host + ':' + std::to_string(port));
    request.set(http::field::authorization, "Bearer " + options().control_token);
    request.set(http::field::content_type, "application/json");
    request.keep_alive(false);
    request.body() = boost::json::serialize(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return 0;
    }
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    co_return response.result_int();
}

}    // namespace

gb_client::gb_client(boost::asio::io_context& io, const std::string& path) : udp_socket_(io), tcp_socket_(io), acceptor_(io)
{
    const auto separator = path.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1U == path.size())
    {
        throw std::runtime_error("GB play path must be stream_id/sender_id");
    }
    key_ = {path.substr(0, separator), path.substr(separator + 1U)};
    ssrc_ = next_ssrc();
    buffer_.resize(receive_buffer_bytes);
}

void gb_client::configure(gb_client_options value) { options() = std::move(value); }

const std::vector<gb_sender_key>& gb_client::created_senders() { return senders(); }

boost::asio::awaitable<bool> gb_client::delete_sender(std::string host, std::uint16_t port, gb_sender_key key)
{
    boost::json::object body{{"stream_id", key.stream_id}, {"sender_id", key.sender_id}};
    const auto status = co_await control_request(std::move(host), port, "/gb28181/sender/delete", std::move(body));
    co_return status == 204U || status == 404U;
}

std::uint64_t gb_client::video_media_ms() const noexcept { return video_timestamps_.advanced() / video_clock_khz; }

boost::asio::awaitable<boost::system::error_code> gb_client::play(std::string host, std::uint16_t port, bool expect_audio)
{
    const auto& settings = options();
    boost::system::error_code error;
    std::uint16_t media_port{};
    if (settings.transport == gb_transport::udp)
    {
        udp_socket_.open(settings.bind_address.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4(), error);
        if (!error)
        {
            udp_socket_.set_option(boost::asio::socket_base::receive_buffer_size(udp_receive_buffer_bytes), error);
        }
        if (!error)
        {
            udp_socket_.bind({settings.bind_address, 0}, error);
        }
        if (error)
        {
            co_return error;
        }
        media_port = udp_socket_.local_endpoint().port();
    }
    else if (settings.transport == gb_transport::tcp_active)
    {
        media_port = next_listen_port();
        const boost::asio::ip::tcp::endpoint endpoint{settings.bind_address, media_port};
        acceptor_.open(endpoint.protocol(), error);
        if (!error)
        {
            acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
        }
        if (!error)
        {
            acceptor_.bind(endpoint, error);
        }
        if (!error)
        {
            acceptor_.listen(1, error);
        }
        if (error)
        {
            co_return error;
        }
    }
    else
    {
        media_port = next_listen_port();
    }

    error = co_await create_sender(host, port, media_port);
    if (error)
    {
        co_return error;
    }
    if (settings.transport == gb_transport::tcp_active)
    {
        co_await acceptor_.async_accept(tcp_socket_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
        acceptor_.close();
    }
    else if (settings.transport == gb_transport::tcp_passive)
    {
        const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address(host, error), media_port};
        if (!error)
        {
            co_await tcp_socket_.async_connect(endpoint, boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
    }
    if (error)
    {
        co_return error;
    }
    handshake_complete_ = true;
    while (video_frames_ == 0 || (expect_audio && audio_frames_ == 0))
    {
        error = co_await receive();
        if (error)
        {
            co_return error;
        }
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> gb_client::consume_one() { co_return co_await receive(); }

void gb_client::cancel() noexcept
{
    boost::system::error_code error;
    udp_socket_.close(error);
    tcp_socket_.close(error);
    acceptor_.close(error);
}

boost::asio::awaitable<boost::system::error_code> gb_client::create_sender(std::string host, std::uint16_t port, std::uint16_t media_port)
{
    const auto& settings = options();
    boost::json::object body{{"stream_id", key_.stream_id},
                             {"sender_id", key_.sender_id},
                             {"transport", transport_name(settings.transport)},
                             {"payload_type", settings.payload_type},
                             {"ssrc", ssrc_}};
    if (settings.transport == gb_transport::udp)
    {
        body["remote_address"] = settings.bind_address.to_string();
        body["remote_rtp_port"] = media_port;
    }
    else if (settings.transport == gb_transport::tcp_active)
    {
        body["remote_address"] = settings.bind_address.to_string();
        body["remote_port"] = media_port;
    }
    else
    {
        body["listen_port"] = media_port;
    }
    senders().push_back(key_);
    const auto status = co_await control_request(std::move(host), port, "/gb28181/sender/create", std::move(body));
    if (status == 403U)
    {
        co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
    }
    co_return status == 201U ? boost::system::error_code{} : boost::asio::error::fault;
}

boost::asio::awaitable<boost::system::error_code> gb_client::receive()
{
    boost::system::error_code error;
    if (options().transport == gb_transport::udp)
    {
        const auto bytes =
            co_await udp_socket_.async_receive(boost::asio::buffer(buffer_), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error)
        {
            co_return error;
        }
        co_return on_packet({buffer_.data(), bytes}) ? boost::system::error_code{} : boost::asio::error::fault;
    }

    const auto bytes = co_await tcp_socket_.async_read_some(boost::asio::buffer(buffer_.data() + buffered_, buffer_.size() - buffered_),
                                                            boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    buffered_ += bytes;
    std::size_t offset{};
    while (buffered_ - offset >= 2U)
    {
        const auto length = static_cast<std::size_t>((buffer_[offset] << 8U) | buffer_[offset + 1U]);
        if (length == 0)
        {
            co_return boost::asio::error::fault;
        }
        if (buffered_ - offset < 2U + length)
        {
            break;
        }
        if (!on_packet({buffer_.data() + offset + 2U, length}))
        {
            co_return boost::asio::error::fault;
        }
        offset += 2U + length;
    }
    std::memmove(buffer_.data(), buffer_.data() + offset, buffered_ - offset);
    buffered_ -= offset;
    co_return boost::system::error_code{};
}

bool gb_client::on_packet(std::span<const std::uint8_t> packet)
{
    if (packet.size() < 12U || (packet[0] >> 6U) != 2U || (packet[1] & 0x7FU) != options().payload_type || read_u32(packet.data() + 8) != ssrc_)
    {
        return false;
    }
    const auto sequence = static_cast<std::uint16_t>((packet[2] << 8U) | packet[3]);
    if (sequence_ && static_cast<std::uint16_t>(sequence - *sequence_) != 1U)
    {
        return false;
    }
    sequence_ = sequence;
    std::size_t offset = 12U + (static_cast<std::size_t>(packet[0] & 0x0FU) * 4U);
    if ((packet[0] & 0x10U) != 0)
    {
        if (packet.size() < offset + 4U)
        {
            return false;
        }
        offset += 4U + (static_cast<std::size_t>((packet[offset + 2] << 8U) | packet[offset + 3]) * 4U);
    }
    if ((packet[0] & 0x20U) != 0 || packet.size() <= offset)
    {
        return false;
    }
    const auto payload = packet.subspan(offset);
    const auto timestamp = read_u32(packet.data() + 4);
    if (start_code(payload, 0) && payload[3] == pack_start)
    {
        const auto frame = parse_ps_frame(payload);
        if (!frame || (frame->pts && !(frame->video ? video_timestamps_ : audio_timestamps_).observe(*frame->pts)))
        {
            return false;
        }
        frame_timestamp_ = timestamp;
        video_frame_ = frame->video;
        ++(video_frame_ ? video_frames_ : audio_frames_);
    }
    else if (!frame_timestamp_ || *frame_timestamp_ != timestamp)
    {
        return false;
    }
    (video_frame_ ? video_bytes_ : audio_bytes_) += payload.size();
    return true;
}

}    // namespace media_server::bench
