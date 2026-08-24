#include <array>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_input_udp_session.h"

namespace media_server
{

rtsp_input_udp_session::rtsp_input_udp_session(std::weak_ptr<rtsp_server_connection> connection,
                                               boost::asio::any_io_executor executor,
                                               stream_registry& registry,
                                               std::string stream_name,
                                               std::string session_id,
                                               std::vector<rtsp_input_track_description> descriptions)
    : connection_(std::move(connection)),
      media_(registry, executor, std::move(stream_name), session_id, std::move(descriptions)),
      session_id_(std::move(session_id)),
      tracks_(media_.descriptions().size()),
      rtcp_timer_(std::move(executor))
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
        return result;
    }

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }
    const auto self = shared_from_this();
    auto handler = std::make_shared<rtsp_server_connection_handler>();
    handler->on_read = [self](std::span<const std::uint8_t> data) { return self->on_read(data); };
    handler->on_shutdown = [self]() { self->safe_shutdown(); };
    handler->on_setup = [self](rtsp_server_t* handler_server,
                               const char* handler_uri,
                               const char* handler_session,
                               const rtsp_header_transport_t handler_transports[],
                               std::size_t handler_count)
    { return self->on_setup(handler_server, handler_uri, handler_session, handler_transports, handler_count); };
    handler->on_teardown = [self](rtsp_server_t* handler_server, const char*, const char* handler_session)
    { return self->on_teardown(handler_server, handler_session); };
    handler->on_announce = [](rtsp_server_t* handler_server, const char*, const char*, int)
    { return rtsp_server_reply_announce(handler_server, 455); };
    handler->on_record = [self](rtsp_server_t* handler_server, const char*, const char* handler_session, const std::int64_t*, const double*)
    { return self->on_record(handler_server, handler_session); };
    handler->on_get_parameter = [](rtsp_server_t* handler_server, const char*, const char*, const void*, int)
    { return rtsp_server_reply_get_parameter(handler_server, 200, nullptr, 0); };
    connection->set_handler(std::move(handler));
    return result;
}

void rtsp_input_udp_session::shutdown()
{
    if (const auto connection = connection_.lock())
    {
        connection->shutdown();
    }
}

std::size_t rtsp_input_udp_session::on_read(std::span<const std::uint8_t> data)
{
    const auto connection = connection_.lock();
    return connection ? connection->input(data) : data.size();
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
        shutdown();
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
        shutdown();
    }
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

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }

    const auto self = shared_from_this();
    std::shared_ptr<udp_socket> rtp_socket;
    std::shared_ptr<udp_socket> rtcp_socket;
    for (std::uint32_t port = 49'152; port <= 65'534; port += 2U)
    {
        auto candidate_rtp = std::make_shared<udp_socket>(connection->executor());
        if (!candidate_rtp->startup(
                boost::asio::ip::address_v4::any(),
                static_cast<std::uint16_t>(port),
                [self, selected_index](
                    boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
                {
                    if (error)
                    {
                        self->shutdown();
                        return;
                    }
                    self->on_rtp(selected_index, data, endpoint);
                },
                [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
                {
                    if (error)
                    {
                        self->shutdown();
                    }
                }))
        {
            continue;
        }

        auto candidate_rtcp = std::make_shared<udp_socket>(connection->executor());
        if (!candidate_rtcp->startup(
                boost::asio::ip::address_v4::any(),
                static_cast<std::uint16_t>(port + 1U),
                [self, selected_index](
                    boost::system::error_code error, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint)
                {
                    if (error)
                    {
                        self->shutdown();
                        return;
                    }
                    self->on_rtcp(selected_index, data, endpoint);
                },
                [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
                {
                    if (error)
                    {
                        self->shutdown();
                    }
                }))
        {
            candidate_rtp->shutdown();
            continue;
        }
        rtp_socket = std::move(candidate_rtp);
        rtcp_socket = std::move(candidate_rtcp);
        break;
    }
    if (!rtp_socket || !rtcp_socket)
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    auto& state = tracks_[selected_index];
    state.rtp_socket = std::move(rtp_socket);
    state.rtcp_socket = std::move(rtcp_socket);
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
        shutdown();
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
    shutdown();
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
        if (state.rtp_socket)
        {
            state.rtp_socket->shutdown();
        }
        if (state.rtcp_socket)
        {
            state.rtcp_socket->shutdown();
        }
        state = {};
    }
    spdlog::debug("rtsp input udp shutdown {}", media_.stream_name());
}

}    // namespace media_server
