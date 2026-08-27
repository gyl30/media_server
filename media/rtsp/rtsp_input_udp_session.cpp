#include <array>
#include <atomic>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_input_udp_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

rtsp_input_udp_session::rtsp_input_udp_session(boost::asio::any_io_executor executor,
                                               std::string stream_name,
                                               std::string session_id,
                                               std::vector<rtsp_input_track_description> descriptions,
                                               std::function<void()> request_shutdown)
    : executor_(std::move(executor)),
      request_shutdown_(std::move(request_shutdown)),
      media_(executor_, std::move(stream_name), session_id, std::move(descriptions)),
      session_id_(std::move(session_id)),
      tracks_(media_.descriptions().size()),
      rtcp_timer_(executor_)
{
}

int rtsp_input_udp_session::startup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (closed_ || !media_.startup())
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    const auto result = on_setup(server, uri, session, transports, count);
    if (result != 0 || !std::ranges::any_of(tracks_, [](const track_state& state) { return state.rtp_socket != nullptr; }))
    {
        safe_shutdown();
        return result;
    }
    return result;
}

void rtsp_input_udp_session::on_rtp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!recording_ || track_index >= tracks_.size() || data.size() < 12)
    {
        return;
    }
    const auto& state = tracks_[track_index];
    if (!state.rtp_socket || endpoint != state.rtp_endpoint)
    {
        return;
    }
    if (!media_.input(track_index, data))
    {
        request_shutdown_();
    }
}

void rtsp_input_udp_session::on_rtcp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!recording_ || track_index >= tracks_.size() || data.size() < 4)
    {
        return;
    }
    const auto& state = tracks_[track_index];
    if (!state.rtcp_socket || endpoint != state.rtcp_endpoint)
    {
        return;
    }
    if (!media_.input(track_index, data))
    {
        request_shutdown_();
    }
}

std::optional<rtsp_input_udp_session::udp_socket_pair> rtsp_input_udp_session::prepare_udp_sockets(std::size_t track_index,
                                                                                                   boost::asio::any_io_executor executor)
{
    const auto self = shared_from_this();
    const auto reserved = port_manager::instance().acquire_pair();
    if (!reserved)
    {
        return std::nullopt;
    }

    const auto local_ports = *reserved;
    auto candidate_rtp = std::make_shared<udp_socket>(executor);
    if (!candidate_rtp->startup(
            boost::asio::ip::address_v4::any(),
            local_ports.first,
            [self, track_index](
                boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (error)
                {
                    self->request_shutdown_();
                    return;
                }
                self->on_rtp(track_index, data, endpoint);
            },
            [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->request_shutdown_();
                }
            }))
    {
        port_manager::instance().release(local_ports);
        return std::nullopt;
    }

    auto candidate_rtcp = std::make_shared<udp_socket>(executor);
    if (!candidate_rtcp->startup(
            boost::asio::ip::address_v4::any(),
            local_ports.second,
            [self, track_index](
                boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (error)
                {
                    self->request_shutdown_();
                    return;
                }
                self->on_rtcp(track_index, data, endpoint);
            },
            [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
            {
                if (error)
                {
                    self->request_shutdown_();
                }
            }))
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
        return std::nullopt;
    }
    return udp_socket_pair{.rtp = std::move(candidate_rtp), .rtcp = std::move(candidate_rtcp), .local_ports = local_ports};
}

int rtsp_input_udp_session::on_setup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (transports == nullptr || count == 0 || (!session.empty() && session != session_id_))
    {
        return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
    }

    const auto& descriptions = media_.descriptions();
    const auto description =
        std::find_if(descriptions.begin(), descriptions.end(), [uri](const rtsp_input_track_description& value) { return uri == value.uri; });
    if (description == descriptions.end())
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto selected_index = static_cast<std::size_t>(description - descriptions.begin());
    if (tracks_[selected_index].rtp_socket)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }

    const rtsp_header_transport_t* transport = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (transports[index].transport == RTSP_TRANSPORT_RTP_UDP && transports[index].multicast == 0 &&
            (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_RECORD))
        {
            transport = &transports[index];
            break;
        }
    }
    if (transport == nullptr || transport->rtp.u.client_port1 == 0 || transport->rtp.u.client_port2 == 0)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
    }

    boost::system::error_code address_error;
    const auto client_address = boost::asio::ip::make_address(rtsp_server_get_client(server, nullptr), address_error);
    if (address_error)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
    }

    auto sockets = prepare_udp_sockets(selected_index, executor_);
    if (!sockets)
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    auto& state = tracks_[selected_index];
    state.rtp_socket = std::move(sockets->rtp);
    state.rtcp_socket = std::move(sockets->rtcp);
    state.local_ports = sockets->local_ports;
    state.rtp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport->rtp.u.client_port1);
    state.rtcp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport->rtp.u.client_port2);

    rtsp_server_set_session_timeout(server, 60);
    const auto response = "RTP/AVP;unicast;client_port=" + std::to_string(transport->rtp.u.client_port1) + "-" +
                          std::to_string(transport->rtp.u.client_port2) + ";server_port=" + std::to_string(state.rtp_socket->local_port()) + "-" +
                          std::to_string(state.rtcp_socket->local_port()) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id_.c_str(), response.c_str());
}

int rtsp_input_udp_session::on_record(rtsp_server_t* server, std::string_view session)
{
    if (recording_ || session != session_id_)
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (std::ranges::any_of(tracks_, [](const track_state& state) { return state.rtp_socket == nullptr; }))
    {
        return rtsp_server_reply_record(server, 455, nullptr, nullptr);
    }
    if (!media_.start_recording())
    {
        rtsp_server_reply_record(server, 453, nullptr, nullptr);
        request_shutdown_();
        return 0;
    }
    recording_ = true;
    wait_rtcp();
    return rtsp_server_reply_record(server, 200, nullptr, nullptr);
}

int rtsp_input_udp_session::on_teardown(rtsp_server_t* server, std::string_view session)
{
    if (session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    request_shutdown_();
    return result;
}

void rtsp_input_udp_session::wait_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || !self->recording_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            for (std::size_t index = 0; index < self->tracks_.size(); ++index)
            {
                auto& state = self->tracks_[index];
                const auto bytes = self->media_.rtcp(index, buffer);
                if (bytes <= 0 || !state.rtcp_socket)
                {
                    continue;
                }
                state.rtcp_socket->send(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytes), state.rtcp_endpoint);
            }
            self->wait_rtcp();
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
    for (auto& state : tracks_)
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
            if (state.rtp_socket)
            {
                state.rtp_socket->shutdown(release);
            }
            else
            {
                release();
            }
            if (state.rtcp_socket)
            {
                state.rtcp_socket->shutdown(release);
            }
            else
            {
                release();
            }
        }
        state = {};
    }
    spdlog::debug("rtsp input udp shutdown {}", media_.stream_name());
}

}    // namespace media_server
