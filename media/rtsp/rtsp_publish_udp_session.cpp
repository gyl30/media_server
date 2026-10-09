#include <span>
#include <array>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_publish_udp_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

rtsp_publish_udp_session::rtsp_publish_udp_session(worker_context& worker,
                                                   boost::asio::ip::address bind_address,
                                                   std::string stream_name,
                                                   std::vector<rtsp_publish_track_description> descriptions,
                                                   std::function<void()> input_handler)
    : worker_(worker),
      bind_address_(std::move(bind_address)),
      input_handler_(std::move(input_handler)),
      media_(worker_, std::move(stream_name), std::move(descriptions)),
      track_states_(media_.descriptions().size()),
      rtcp_timer_(worker_.io())
{
}

int rtsp_publish_udp_session::startup(rtsp_server_t* server,
                                      std::size_t track_index,
                                      const rtsp_header_transport_t& transport,
                                      const std::string& session_id)
{
    if (!media_.startup(session_id))
    {
        return -1;
    }

    return on_setup(server, track_index, transport, session_id);
}

void rtsp_publish_udp_session::run_rtp(std::size_t track_index, boost::asio::yield_context yield)
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
            if (error != boost::asio::error::operation_aborted && shutdown_handler_)
            {
                shutdown_handler_();
            }
            return;
        }
        input_handler_();
        if (bytes < 12)
        {
            continue;
        }
        if (!media_.input_packet(track_index, std::span<const std::uint8_t>{buffer.data(), bytes}))
        {
            if (shutdown_handler_)
            {
                shutdown_handler_();
            }
            return;
        }
    }
}

void rtsp_publish_udp_session::run_rtcp(std::size_t track_index, boost::asio::yield_context yield)
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
            if (error != boost::asio::error::operation_aborted && shutdown_handler_)
            {
                shutdown_handler_();
            }
            return;
        }
        if (bytes < 4)
        {
            continue;
        }
        if (!media_.input_packet(track_index, std::span<const std::uint8_t>{buffer.data(), bytes}))
        {
            if (shutdown_handler_)
            {
                shutdown_handler_();
            }
            return;
        }
    }
}

int rtsp_publish_udp_session::on_setup(rtsp_server_t* server,
                                       std::size_t track_index,
                                       const rtsp_header_transport_t& transport,
                                       const std::string& session_id)
{
    auto& state = track_states_[track_index];
    if (state.local_port)
    {
        spdlog::debug("rtsp publish udp track already setup {}", track_index);
        return -1;
    }
    boost::system::error_code address_error;
    const auto client_address = boost::asio::ip::make_address(rtsp_server_get_client(server, nullptr), address_error);
    if (address_error)
    {
        return -1;
    }

    state.rtp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port1);
    state.rtcp_endpoint = boost::asio::ip::udp::endpoint(client_address, transport.rtp.u.client_port2);
    state.rtp_transport = std::make_shared<udp_transport>(worker_.io());
    state.rtcp_transport = std::make_shared<udp_transport>(worker_.io());

    // 失败返回 -1 使连接关闭，端口与 transport 由 safe_shutdown 清理。
    state.local_port = media_port_pool::instance().acquire();
    if (!state.local_port)
    {
        return -1;
    }
    const auto local_port = *state.local_port;

    boost::system::error_code network_error;
    state.rtp_transport->startup(bind_address_, local_port, network_error);
    if (!network_error)
    {
        state.rtcp_transport->startup(bind_address_, static_cast<std::uint16_t>(local_port + 1U), network_error);
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
        return -1;
    }

    const auto self = shared_from_this();
    state.rtcp_transport->set_write_callback(
        [weak = weak_from_this(), track_index](boost::system::error_code error, std::size_t)
        {
            const auto locked = weak.lock();
            if (!locked || !locked->track_states_[track_index].local_port)
            {
                return;
            }
            if (error)
            {
                if (error != boost::asio::error::operation_aborted && locked->shutdown_handler_)
                {
                    locked->shutdown_handler_();
                }
                return;
            }
            locked->send_rtcp(track_index + 1U);
        });
    worker_.spawn([self, track_index](boost::asio::yield_context yield) { self->run_rtp(track_index, yield); });
    worker_.spawn([self, track_index](boost::asio::yield_context yield) { self->run_rtcp(track_index, yield); });

    const auto response = "RTP/AVP;unicast;client_port=" + std::to_string(transport.rtp.u.client_port1) + "-" +
                          std::to_string(transport.rtp.u.client_port2) + ";server_port=" + std::to_string(local_port) + "-" +
                          std::to_string(local_port + 1U) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id.c_str(), response.c_str());
}

int rtsp_publish_udp_session::on_record(rtsp_server_t* server)
{
    if (std::ranges::any_of(track_states_, [](const track_state& state) { return !state.local_port; }))
    {
        spdlog::debug("rtsp publish udp record before all tracks setup");
        return -1;
    }
    if (!media_.start_recording())
    {
        spdlog::debug("rtsp publish udp start recording failed");
        return -1;
    }

    schedule_rtcp();
    return rtsp_server_reply_record(server, 200, nullptr, nullptr);
}

void rtsp_publish_udp_session::schedule_rtcp()
{
    if (std::ranges::any_of(track_states_, [](const track_state& state) { return !state.local_port; }))
    {
        return;
    }

    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error)
            {
                return;
            }

            self->send_rtcp(0);
        });
}

void rtsp_publish_udp_session::send_rtcp(std::size_t track_index)
{
    std::array<std::uint8_t, 1500> buffer{};
    for (std::size_t index = track_index; index < track_states_.size(); ++index)
    {
        auto& state = track_states_[index];
        const auto bytes = media_.generate_rtcp(index, buffer);
        if (bytes <= 0)
        {
            continue;
        }

        if (state.rtcp_transport->write(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(bytes)}, state.rtcp_endpoint))
        {
            return;
        }
    }

    schedule_rtcp();
}

void rtsp_publish_udp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtsp_publish_udp_session::safe_shutdown()
{
    rtcp_timer_.cancel();
    media_.shutdown();
    shutdown_handler_ = {};
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
        if (state.local_port)
        {
            media_port_pool::instance().release(*state.local_port);
            state.local_port.reset();
        }
    }
    spdlog::debug("rtsp publish udp shutdown {}", media_.media_stream_name());
}

}    // namespace media_server
