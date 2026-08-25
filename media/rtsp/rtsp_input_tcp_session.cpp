#include <array>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_input_tcp_session.h"

namespace media_server
{

rtsp_input_tcp_session::rtsp_input_tcp_session(std::weak_ptr<rtsp_server_connection> connection,
                                               boost::asio::any_io_executor executor,
                                               std::string stream_name,
                                               std::string session_id,
                                               std::vector<rtsp_input_track_description> descriptions)
    : connection_(std::move(connection)),
      media_(executor, std::move(stream_name), session_id, std::move(descriptions)),
      session_id_(std::move(session_id)),
      tracks_(media_.descriptions().size()),
      rtcp_timer_(std::move(executor))
{
}

rtsp_input_tcp_session::~rtsp_input_tcp_session()
{
    if (interleaved_.data != nullptr)
    {
        std::free(interleaved_.data);
        interleaved_.data = nullptr;
    }
}

int rtsp_input_tcp_session::startup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (closed_ || !media_.startup())
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }
    interleaved_.onrtp = &rtsp_input_tcp_session::rtp_callback;
    interleaved_.param = this;

    const auto result = on_setup(server, uri, session, transports, count);
    if (result != 0 || !std::ranges::any_of(tracks_, [](const track_state& state) { return state.rtp_channel >= 0; }))
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

void rtsp_input_tcp_session::shutdown()
{
    if (const auto connection = connection_.lock())
    {
        connection->shutdown();
    }
}

void rtsp_input_tcp_session::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    static_cast<rtsp_input_tcp_session*>(param)->on_rtp(channel, data, bytes);
}

std::size_t rtsp_input_tcp_session::on_read(std::span<const std::uint8_t> data)
{
    if (closed_ || data.empty())
    {
        return data.size();
    }

    if (interleaved_.state != 0 || data.front() == '$')
    {
        const auto* next = rtp_over_rtsp(&interleaved_, data.data(), data.data() + data.size());
        if (next == data.data())
        {
            shutdown();
            return data.size();
        }
        return static_cast<std::size_t>(next - data.data());
    }

    const auto connection = connection_.lock();
    return connection ? connection->input(data) : data.size();
}

void rtsp_input_tcp_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    if (!recording_ || data == nullptr)
    {
        return;
    }
    for (std::size_t index = 0; index < tracks_.size(); ++index)
    {
        if (tracks_[index].rtp_channel == channel || tracks_[index].rtcp_channel == channel)
        {
            if (!media_.input(index, std::span(static_cast<const std::uint8_t*>(data), bytes)))
            {
                shutdown();
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
        shutdown();
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
    shutdown();
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

            const auto connection = self->connection_.lock();
            if (!connection)
            {
                self->shutdown();
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
                connection->write(packet);
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
    if (interleaved_.data != nullptr)
    {
        std::free(interleaved_.data);
        interleaved_.data = nullptr;
    }
    interleaved_.capacity = 0;
    interleaved_.bytes = 0;
    interleaved_.length = 0;
    interleaved_.state = 0;
    spdlog::debug("rtsp input tcp shutdown {}", media_.stream_name());
}

}    // namespace media_server
