#include <array>
#include <chrono>
#include <span>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>

#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_input_udp_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

rtsp_input_udp_session::rtsp_input_udp_session(worker_context& worker,
                                               boost::asio::ip::address bind_address,
                                               std::string stream_name,
                                               std::vector<rtsp_input_track_description> descriptions)
    : worker_(worker),
      bind_address_(std::move(bind_address)),
      media_(worker_, std::move(stream_name), std::move(descriptions)),
      track_states_(media_.descriptions().size()),
      rtcp_timer_(worker_.io())
{
}

int rtsp_input_udp_session::startup(rtsp_server_t* server,
                                    std::size_t track_index,
                                    const rtsp_header_transport_t& transport,
                                    const std::string& session_id)
{
    if (!media_.startup(session_id))
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    const auto result = on_setup(server, track_index, transport, session_id);
    if (result != 0)
    {
        safe_shutdown();
    }
    return result;
}

void rtsp_input_udp_session::run_rtp(std::size_t track_index, boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    auto& transport = *track_states_[track_index].rtp_transport;
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport.read(buffer, endpoint, yield, error);
        if (error)
        {
            if (error != boost::asio::error::operation_aborted && error_handler_)
            {
                error_handler_(error);
            }
            return;
        }
        if (bytes < 12)
        {
            continue;
        }
        if (!media_.input_packet(track_index, std::span<const std::uint8_t>{buffer.data(), bytes}))
        {
            if (error_handler_)
            {
                error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
            }
            return;
        }
    }
}

void rtsp_input_udp_session::run_rtcp(std::size_t track_index, boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    auto& transport = *track_states_[track_index].rtcp_transport;
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport.read(buffer, endpoint, yield, error);
        if (error)
        {
            if (error != boost::asio::error::operation_aborted && error_handler_)
            {
                error_handler_(error);
            }
            return;
        }
        if (bytes < 4)
        {
            continue;
        }
        if (!media_.input_packet(track_index, std::span<const std::uint8_t>{buffer.data(), bytes}))
        {
            if (error_handler_)
            {
                error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
            }
            return;
        }
    }
}

int rtsp_input_udp_session::on_setup(rtsp_server_t* server,
                                     std::size_t track_index,
                                     const rtsp_header_transport_t& transport,
                                     const std::string& session_id)
{
    auto& state = track_states_[track_index];
    if (state.local_ports)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    boost::system::error_code address_error;
    const auto client_address = boost::asio::ip::make_address(rtsp_server_get_client(server, nullptr), address_error);
    if (address_error)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
    }

    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }
    const auto local_ports = *reserved;
    state.rtp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port1);
    state.rtcp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port2);
    state.rtp_transport.emplace(worker_.io());
    state.rtcp_transport.emplace(worker_.io());

    const auto cleanup = [&]
    {
        state.rtp_transport->shutdown();
        state.rtcp_transport->shutdown();
        state.rtp_transport.reset();
        state.rtcp_transport.reset();
        state.rtp_endpoint = {};
        state.rtcp_endpoint = {};
        port_manager::instance().release(local_ports);
    };

    boost::system::error_code network_error;
    state.rtp_transport->startup(bind_address_, local_ports.first, network_error);
    if (!network_error)
    {
        state.rtcp_transport->startup(bind_address_, local_ports.second, network_error);
    }
    if (!network_error)
    {
        state.rtp_transport->connect(state.rtp_endpoint, network_error);
    }
    if (!network_error)
    {
        state.rtcp_transport->connect(state.rtcp_endpoint, network_error);
    }
    if (network_error)
    {
        cleanup();
        error_handler_(network_error);
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }
    state.local_ports = local_ports;

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self, track_index](boost::asio::yield_context yield) { self->run_rtp(track_index, yield); }, boost::asio::detached);
    boost::asio::spawn(worker_.io(), [self, track_index](boost::asio::yield_context yield) { self->run_rtcp(track_index, yield); }, boost::asio::detached);

    rtsp_server_set_session_timeout(server, 60);
    const auto response = "RTP/AVP;unicast;client_port=" + std::to_string(transport.rtp.u.client_port1) + "-" +
                          std::to_string(transport.rtp.u.client_port2) + ";server_port=" + std::to_string(local_ports.first) + "-" +
                          std::to_string(local_ports.second) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id.c_str(), response.c_str());
}

int rtsp_input_udp_session::on_record(rtsp_server_t* server)
{
    if (media_.recording())
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (std::ranges::any_of(track_states_, [](const track_state& state) { return !state.local_ports; }))
    {
        return rtsp_server_reply_record(server, 455, nullptr, nullptr);
    }
    if (!media_.start_recording())
    {
        rtsp_server_reply_record(server, 453, nullptr, nullptr);
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return 0;
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_rtcp_sender(yield); }, boost::asio::detached);
    return rtsp_server_reply_record(server, 200, nullptr, nullptr);
}

void rtsp_input_udp_session::run_rtcp_sender(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    for (;;)
    {
        rtcp_timer_.expires_after(std::chrono::seconds(1));
        rtcp_timer_.async_wait(yield[error]);
        if (error)
        {
            return;
        }

        std::array<std::uint8_t, 1500> buffer{};
        for (std::size_t index = 0; index < track_states_.size(); ++index)
        {
            auto& state = track_states_[index];
            const auto bytes = media_.generate_rtcp(index, buffer);
            if (bytes <= 0)
            {
                continue;
            }

            static_cast<void>(state.rtcp_transport->write(
                std::span{buffer.data(), static_cast<std::size_t>(bytes)}, state.rtcp_endpoint, yield, error));
            if (error)
            {
                if (error != boost::asio::error::operation_aborted && error_handler_)
                {
                    error_handler_(error);
                }
                return;
            }
        }
    }
}

void rtsp_input_udp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    rtcp_timer_.cancel();
    media_.shutdown();
    error_handler_ = {};
    for (auto& state : track_states_)
    {
        if (state.rtp_transport)
        {
            state.rtp_transport->shutdown();
        }
        if (state.rtcp_transport)
        {
            state.rtcp_transport->shutdown();
        }
        if (state.local_ports)
        {
            port_manager::instance().release(*state.local_ports);
            state.local_ports.reset();
        }
    }
    spdlog::debug("rtsp input udp shutdown {}", media_.stream_name());
}

}    // namespace media_server
