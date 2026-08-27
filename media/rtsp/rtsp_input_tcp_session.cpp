#include <array>
#include <chrono>
#include <utility>
#include <algorithm>

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

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
                                               std::string session_id,
                                               std::vector<rtsp_input_track_description> descriptions,
                                               std::function<void(std::span<const std::uint8_t>)> write)
    : write_(std::move(write)),
      media_(executor, std::move(stream_name), session_id, std::move(descriptions)),
      session_id_(std::move(session_id)),
      tracks_(media_.descriptions().size()),
      rtcp_timer_(std::move(executor))
{
}

rtsp_input_tcp_session::~rtsp_input_tcp_session() = default;

int rtsp_input_tcp_session::startup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (closed_ || !media_.startup())
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }
    const auto result = on_setup(server, uri, session, transports, count);
    if (result != 0 || !std::ranges::any_of(tracks_, [](const track_state& state) { return state.rtp_channel >= 0; }))
    {
        safe_shutdown();
        return result;
    }
    return result;
}

void rtsp_input_tcp_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (!recording_ || data.empty())
    {
        return;
    }
    for (std::size_t index = 0; index < tracks_.size(); ++index)
    {
        if (tracks_[index].rtp_channel == channel || tracks_[index].rtcp_channel == channel)
        {
            if (!media_.input(index, data))
            {
                error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
            }
            return;
        }
    }
}

int rtsp_input_tcp_session::on_setup(
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
    if (tracks_[selected_index].rtp_channel >= 0)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }

    const rtsp_header_transport_t* transport = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        const bool valid_interleaved = transports[index].interleaved1 >= 0 && transports[index].interleaved2 >= 0 &&
                                       transports[index].interleaved1 <= 255 && transports[index].interleaved2 <= 255 &&
                                       transports[index].interleaved1 != transports[index].interleaved2;
        if (transports[index].transport == RTSP_TRANSPORT_RTP_TCP && transports[index].multicast == 0 &&
            (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_RECORD) && valid_interleaved)
        {
            transport = &transports[index];
            break;
        }
    }
    if (transport == nullptr)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    for (const auto& state : tracks_)
    {
        if (state.rtp_channel >= 0 && (state.rtp_channel == transport->interleaved1 || state.rtp_channel == transport->interleaved2 ||
                                       state.rtcp_channel == transport->interleaved1 || state.rtcp_channel == transport->interleaved2))
        {
            return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
        }
    }

    auto& state = tracks_[selected_index];
    state.rtp_channel = transport->interleaved1;
    state.rtcp_channel = transport->interleaved2;
    rtsp_server_set_session_timeout(server, 60);
    const auto response =
        "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(state.rtp_channel) + "-" + std::to_string(state.rtcp_channel) + ";mode=record";
    return rtsp_server_reply_setup(server, 200, session_id_.c_str(), response.c_str());
}

int rtsp_input_tcp_session::on_record(rtsp_server_t* server, std::string_view session)
{
    if (recording_ || session != session_id_)
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (std::ranges::any_of(tracks_, [](const track_state& state) { return state.rtp_channel < 0; }))
    {
        return rtsp_server_reply_record(server, 455, nullptr, nullptr);
    }
    if (!media_.start_recording())
    {
        rtsp_server_reply_record(server, 453, nullptr, nullptr);
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return 0;
    }
    recording_ = true;
    wait_rtcp();
    return rtsp_server_reply_record(server, 200, nullptr, nullptr);
}

int rtsp_input_tcp_session::on_teardown(rtsp_server_t* server, std::string_view session)
{
    if (session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    error_handle_(boost::asio::error::eof);
    return result;
}

void rtsp_input_tcp_session::wait_rtcp()
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
                const auto bytes = self->media_.rtcp(index, buffer);
                if (bytes <= 0)
                {
                    continue;
                }

                std::vector<std::uint8_t> packet(static_cast<std::size_t>(bytes) + 4U);
                packet[0] = '$';
                packet[1] = static_cast<std::uint8_t>(self->tracks_[index].rtcp_channel);
                packet[2] = static_cast<std::uint8_t>(bytes >> 8U);
                packet[3] = static_cast<std::uint8_t>(bytes);
                std::copy_n(buffer.begin(), bytes, packet.begin() + 4);
                self->write_(packet);
            }
            self->wait_rtcp();
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
    write_ = {};
    error_handle_ = {};
    spdlog::debug("rtsp input tcp shutdown {}", media_.stream_name());
}

}    // namespace media_server
