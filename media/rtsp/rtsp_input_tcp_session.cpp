#include <array>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_input_tcp_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

rtsp_input_tcp_session::rtsp_input_tcp_session(boost::asio::any_io_executor executor,
                                               std::string stream_name,
                                               std::vector<rtsp_input_track_description> descriptions,
                                               std::function<void(std::span<const std::uint8_t>)> write)
    : write_handler_(std::move(write)),
      media_(executor, std::move(stream_name), std::move(descriptions)),
      track_states_(media_.descriptions().size()),
      rtcp_timer_(std::move(executor))
{
}

rtsp_input_tcp_session::~rtsp_input_tcp_session() = default;

int rtsp_input_tcp_session::startup(rtsp_server_t* server,
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

void rtsp_input_tcp_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (data.empty())
    {
        return;
    }
    for (std::size_t index = 0; index < track_states_.size(); ++index)
    {
        if (track_states_[index].rtp_channel == channel || track_states_[index].rtcp_channel == channel)
        {
            if (!media_.input_packet(index, data))
            {
                error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
            }
            return;
        }
    }
}

int rtsp_input_tcp_session::on_setup(rtsp_server_t* server,
                                     std::size_t track_index,
                                     const rtsp_header_transport_t& transport,
                                     const std::string& session_id)
{
    if (track_states_[track_index].rtp_channel >= 0)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }

    for (const auto& state : track_states_)
    {
        if (state.rtp_channel >= 0 && (state.rtp_channel == transport.interleaved1 || state.rtp_channel == transport.interleaved2 ||
                                       state.rtcp_channel == transport.interleaved1 || state.rtcp_channel == transport.interleaved2))
        {
            return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
        }
    }

    auto& state = track_states_[track_index];
    state.rtp_channel = transport.interleaved1;
    state.rtcp_channel = transport.interleaved2;
    rtsp_server_set_session_timeout(server, 60);
    const auto response =
        "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(state.rtp_channel) + "-" + std::to_string(state.rtcp_channel) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id.c_str(), response.c_str());
}

int rtsp_input_tcp_session::on_record(rtsp_server_t* server)
{
    if (media_.recording())
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (std::ranges::any_of(track_states_, [](const track_state& state) { return state.rtp_channel < 0; }))
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

void rtsp_input_tcp_session::schedule_rtcp()
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
                const auto bytes = self->media_.generate_rtcp(index, buffer);
                if (bytes <= 0)
                {
                    continue;
                }

                std::vector<std::uint8_t> packet(static_cast<std::size_t>(bytes) + 4U);
                packet[0] = '$';
                packet[1] = static_cast<std::uint8_t>(self->track_states_[index].rtcp_channel);
                packet[2] = static_cast<std::uint8_t>(bytes >> 8U);
                packet[3] = static_cast<std::uint8_t>(bytes);
                std::copy_n(buffer.begin(), bytes, packet.begin() + 4);
                self->write_handler_(packet);
            }
            self->schedule_rtcp();
        });
}

void rtsp_input_tcp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    rtcp_timer_.cancel();
    media_.shutdown();
    write_handler_ = {};
    error_handler_ = {};
    spdlog::debug("rtsp input tcp shutdown {}", media_.stream_name());
}

}    // namespace media_server
