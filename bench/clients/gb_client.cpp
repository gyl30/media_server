#include <array>
#include <chrono>
#include <random>
#include <cstring>
#include <utility>

#include <simdjson.h>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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
constexpr auto feedback_interval = std::chrono::seconds{1};
constexpr std::uint8_t rtcp_sender_report = 200;
constexpr std::uint8_t rtcp_receiver_report = 201;
constexpr std::uint8_t rtcp_source_description = 202;

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

std::vector<std::uint64_t>& create_latencies()
{
    static std::vector<std::uint64_t> value;
    return value;
}

std::vector<std::uint64_t>& delete_latencies()
{
    static std::vector<std::uint64_t> value;
    return value;
}

std::ofstream& dump_stream()
{
    static std::ofstream value;
    return value;
}

const void*& dump_owner()
{
    static const void* value{};
    return value;
}

gb_peer_counters& counters()
{
    static gb_peer_counters value;
    return value;
}

void write_u32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

std::array<std::uint8_t, 52> receiver_report(std::uint32_t reporter, std::uint32_t source, std::uint32_t highest_sequence)
{
    std::array<std::uint8_t, 52> packet{};
    packet[0] = 0x81;
    packet[1] = rtcp_receiver_report;
    packet[3] = 7;
    write_u32(packet.data() + 4, reporter);
    write_u32(packet.data() + 8, source);
    write_u32(packet.data() + 16, highest_sequence);
    packet[32] = 0x81;
    packet[33] = rtcp_source_description;
    packet[35] = 4;
    write_u32(packet.data() + 36, reporter);
    packet[40] = 1;
    packet[41] = 8;
    std::memcpy(packet.data() + 42, "gbclient", 8);
    return packet;
}

std::uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point started)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
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

boost::asio::awaitable<unsigned> control_request(std::string host, std::uint16_t port, std::string target, std::string body)
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
    request.body() = std::move(body);
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

gb_client::gb_client(boost::asio::io_context& io, const std::string& path)
    : udp_socket_(io), rtcp_socket_(io), feedback_timer_(io), tcp_socket_(io), acceptor_(io)
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

void gb_client::configure(gb_client_options value)
{
    options() = std::move(value);
    if (!options().dump_file.empty())
    {
        dump_stream().open(options().dump_file, std::ios::binary);
    }
}

const std::vector<gb_sender_key>& gb_client::created_senders() { return senders(); }

const std::vector<std::uint64_t>& gb_client::create_milliseconds() { return create_latencies(); }

const std::vector<std::uint64_t>& gb_client::delete_milliseconds() { return delete_latencies(); }

const gb_peer_counters& gb_client::peer_counters() { return counters(); }

boost::asio::awaitable<bool> gb_client::delete_sender(std::string host, std::uint16_t port, gb_sender_key key)
{
    simdjson::builder::string_builder body;
    body.start_object();
    body.append_key_value("stream_id", key.stream_id);
    body.append_comma();
    body.append_key_value("sender_id", key.sender_id);
    body.end_object();
    const auto started = std::chrono::steady_clock::now();
    const auto status = co_await control_request(std::move(host), port, "/gb28181/sender/delete", std::string{body.view().value_unsafe()});
    delete_latencies().push_back(elapsed_milliseconds(started));
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
        if (!error && settings.peer != gb_peer::quiet && settings.peer != gb_peer::rtp_noise)
        {
            rtcp_socket_.open(udp_socket_.local_endpoint().protocol(), error);
            if (!error)
            {
                rtcp_socket_.bind({settings.bind_address, 0}, error);
            }
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
    if (settings.transport == gb_transport::udp && (settings.peer == gb_peer::rtcp || settings.peer == gb_peer::rtcp_rr))
    {
        boost::asio::co_spawn(udp_socket_.get_executor(), receive_rtcp(), boost::asio::detached);
    }
    if (settings.transport == gb_transport::udp && settings.peer != gb_peer::quiet && settings.peer != gb_peer::rtcp)
    {
        boost::asio::co_spawn(udp_socket_.get_executor(), send_feedback(), boost::asio::detached);
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<void> gb_client::send_feedback()
{
    const bool noise = options().peer == gb_peer::rtp_noise;
    for (;;)
    {
        boost::system::error_code error;
        feedback_timer_.expires_after(feedback_interval);
        co_await feedback_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || !udp_socket_.is_open())
        {
            co_return;
        }
        if (noise)
        {
            std::array<std::uint8_t, 52> datagram{};
            udp_socket_.send_to(boost::asio::buffer(datagram), media_source_, 0, error);
            ++counters().noise_datagrams;
            continue;
        }
        const auto report = receiver_report(ssrc_ ^ 0x80000000U, ssrc_, sequence_.value_or(0));
        const boost::asio::ip::udp::endpoint target{media_source_.address(), static_cast<std::uint16_t>(media_source_.port() + 1U)};
        rtcp_socket_.send_to(boost::asio::buffer(report), target, 0, error);
        ++counters().receiver_reports;
    }
}

boost::asio::awaitable<void> gb_client::receive_rtcp()
{
    std::array<std::uint8_t, 1500> buffer{};
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes =
            co_await rtcp_socket_.async_receive(boost::asio::buffer(buffer), boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error)
        {
            co_return;
        }
        const bool sender_report = bytes >= 28U && (buffer[0] >> 6U) == 2U && buffer[1] == rtcp_sender_report &&
                                   read_u32(buffer.data() + 4) == ssrc_ && read_u32(buffer.data() + 20) != 0;
        ++(sender_report ? counters().sender_reports : counters().invalid_rtcp);
    }
}

boost::asio::awaitable<boost::system::error_code> gb_client::consume_one() { co_return co_await receive(); }

void gb_client::cancel() noexcept
{
    boost::system::error_code error;
    udp_socket_.close(error);
    rtcp_socket_.close(error);
    feedback_timer_.cancel();
    tcp_socket_.close(error);
    acceptor_.close(error);
}

boost::asio::awaitable<boost::system::error_code> gb_client::create_sender(std::string host, std::uint16_t port, std::uint16_t media_port)
{
    const auto& settings = options();
    simdjson::builder::string_builder body;
    body.start_object();
    body.append_key_value("stream_id", key_.stream_id);
    body.append_comma();
    body.append_key_value("sender_id", key_.sender_id);
    body.append_comma();
    body.append_key_value("transport", transport_name(settings.transport));
    body.append_comma();
    body.append_key_value("payload_type", settings.payload_type);
    body.append_comma();
    body.append_key_value("ssrc", ssrc_);
    body.append_comma();
    if (settings.transport == gb_transport::udp)
    {
        body.append_key_value("remote_address", settings.bind_address.to_string());
        body.append_comma();
        body.append_key_value("remote_rtp_port", media_port);
        if (settings.peer == gb_peer::rtcp || settings.peer == gb_peer::rtcp_rr)
        {
            body.append_comma();
            body.append_key_value("remote_rtcp_port", rtcp_socket_.local_endpoint().port());
        }
    }
    else if (settings.transport == gb_transport::tcp_active)
    {
        body.append_key_value("remote_address", settings.bind_address.to_string());
        body.append_comma();
        body.append_key_value("remote_port", media_port);
    }
    else
    {
        body.append_key_value("listen_port", media_port);
    }
    body.end_object();
    senders().push_back(key_);
    const auto started = std::chrono::steady_clock::now();
    const auto status = co_await control_request(std::move(host), port, "/gb28181/sender/create", std::string{body.view().value_unsafe()});
    create_latencies().push_back(elapsed_milliseconds(started));
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
        const auto bytes = co_await udp_socket_.async_receive_from(
            boost::asio::buffer(buffer_), media_source_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
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
    if (dump_stream().is_open() && (dump_owner() == nullptr || dump_owner() == this))
    {
        dump_owner() = this;
        dump_stream().write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    return true;
}

}    // namespace media_server::bench
