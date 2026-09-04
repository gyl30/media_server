#include <array>
#include <atomic>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

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

void rtsp_input_udp_session::on_rtp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (track_index >= track_states_.size() || data.size() < 12)
    {
        return;
    }
    const auto& state = track_states_[track_index];
    if (!state.rtp_socket || endpoint != state.rtp_endpoint)
    {
        return;
    }
    if (!media_.input_packet(track_index, data))
    {
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
    }
}

void rtsp_input_udp_session::on_rtcp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (track_index >= track_states_.size() || data.size() < 4)
    {
        return;
    }
    const auto& state = track_states_[track_index];
    if (!state.rtcp_socket || endpoint != state.rtcp_endpoint)
    {
        return;
    }
    if (!media_.input_packet(track_index, data))
    {
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
    }
}

std::optional<rtsp_input_udp_session::udp_socket_pair> rtsp_input_udp_session::prepare_udp_sockets(std::size_t track_index)
{
    const auto self = shared_from_this();
    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return std::nullopt;
    }

    const auto local_ports = *reserved;
    boost::system::error_code network_error;
    auto candidate_rtp = std::make_shared<udp_socket>(worker_.io());
    candidate_rtp->startup(
        bind_address_,
        local_ports.first,
        [self, track_index](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
        {
            if (self->closed_)
            {
                return;
            }
            if (error)
            {
                self->error_handler_(error);
                return;
            }
            self->on_rtp(track_index, data, endpoint);
        },
        [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
        {
            if (self->closed_)
            {
                return;
            }
            if (error)
            {
                self->error_handler_(error);
            }
        },
        network_error);
    if (network_error)
    {
        port_manager::instance().release(local_ports);
        error_handler_(network_error);
        return std::nullopt;
    }

    auto candidate_rtcp = std::make_shared<udp_socket>(worker_.io());
    candidate_rtcp->startup(
        bind_address_,
        local_ports.second,
        [self, track_index](boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
        {
            if (self->closed_)
            {
                return;
            }
            if (error)
            {
                self->error_handler_(error);
                return;
            }
            self->on_rtcp(track_index, data, endpoint);
        },
        [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
        {
            if (self->closed_)
            {
                return;
            }
            if (error)
            {
                self->error_handler_(error);
            }
        },
        network_error);
    if (network_error)
    {
        auto remaining = std::make_shared<std::atomic_uint8_t>(1);
        candidate_rtp->shutdown(
            [local_ports, remaining]
            {
                if (remaining->fetch_sub(1U, std::memory_order_acq_rel) == 1U)
                {
                    port_manager::instance().release(local_ports);
                }
            });
        error_handler_(network_error);
        return std::nullopt;
    }
    return udp_socket_pair{.rtp = std::move(candidate_rtp), .rtcp = std::move(candidate_rtcp), .local_ports = local_ports};
}

int rtsp_input_udp_session::on_setup(rtsp_server_t* server,
                                     std::size_t track_index,
                                     const rtsp_header_transport_t& transport,
                                     const std::string& session_id)
{
    if (track_states_[track_index].rtp_socket)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    boost::system::error_code address_error;
    const auto client_address = boost::asio::ip::make_address(rtsp_server_get_client(server, nullptr), address_error);
    if (address_error)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
    }

    auto sockets = prepare_udp_sockets(track_index);
    if (!sockets)
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    auto& state = track_states_[track_index];
    state.rtp_socket = std::move(sockets->rtp);
    state.rtcp_socket = std::move(sockets->rtcp);
    state.local_ports = sockets->local_ports;
    state.rtp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port1);
    state.rtcp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port2);

    rtsp_server_set_session_timeout(server, 60);
    const auto response = "RTP/AVP;unicast;client_port=" + std::to_string(transport.rtp.u.client_port1) + "-" +
                          std::to_string(transport.rtp.u.client_port2) + ";server_port=" + std::to_string(state.rtp_socket->local_port()) + "-" +
                          std::to_string(state.rtcp_socket->local_port()) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id.c_str(), response.c_str());
}

int rtsp_input_udp_session::on_record(rtsp_server_t* server)
{
    if (media_.recording())
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (std::ranges::any_of(track_states_, [](const track_state& state) { return state.rtp_socket == nullptr; }))
    {
        return rtsp_server_reply_record(server, 455, nullptr, nullptr);
    }
    if (!media_.start_recording())
    {
        rtsp_server_reply_record(server, 453, nullptr, nullptr);
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return 0;
    }
    schedule_rtcp();
    return rtsp_server_reply_record(server, 200, nullptr, nullptr);
}

void rtsp_input_udp_session::schedule_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            for (std::size_t index = 0; index < self->track_states_.size(); ++index)
            {
                auto& state = self->track_states_[index];
                const auto bytes = self->media_.generate_rtcp(index, buffer);
                if (bytes <= 0)
                {
                    continue;
                }
                state.rtcp_socket->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), state.rtcp_endpoint);
            }
            self->schedule_rtcp();
        });
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
        if (state.local_ports)
        {
            const auto local_ports = *state.local_ports;
            auto remaining = std::make_shared<std::atomic_uint8_t>(2);
            const auto release = [local_ports, remaining]
            {
                if (remaining->fetch_sub(1U, std::memory_order_acq_rel) == 1U)
                {
                    port_manager::instance().release(local_ports);
                }
            };
            state.rtp_socket->shutdown(release);
            state.rtcp_socket->shutdown(release);
        }
        state = {};
    }
    spdlog::debug("rtsp input udp shutdown {}", media_.stream_name());
}

}    // namespace media_server
