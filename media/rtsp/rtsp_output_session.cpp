#include "media/rtsp/rtsp_output_session.h"

#include "media/codec/codec_utils.h"
#include "media/core/log.h"

extern "C"
{
#include "rtp-profile.h"
#include "rtsp-header-transport.h"
#include "rtsp-muxer.h"
#include "rtsp-server.h"
}

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <boost/url/parse.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <random>
#include <sstream>
#include <utility>

namespace media_server
{

namespace
{
std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

std::string local_ip(const tcp_connection& connection)
{
    boost::system::error_code error;
    const auto endpoint = const_cast<tcp_connection&>(connection).socket().local_endpoint(error);
    return error ? "0.0.0.0" : endpoint.address().to_string();
}
}    // namespace

rtsp_output_session::rtsp_output_session(
    std::shared_ptr<tcp_connection> connection,
    stream_registry& registry,
    std::uint16_t server_port)
    : connection_(std::move(connection)), registry_(registry), server_port_(server_port)
{
}

rtsp_output_session::~rtsp_output_session()
{
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
    }
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
    }
}

void rtsp_output_session::start()
{
    rtsp_handler_t handler{};
    handler.close = &rtsp_output_session::close_callback;
    handler.send = &rtsp_output_session::send_callback;
    handler.ondescribe = &rtsp_output_session::describe_callback;
    handler.onsetup = &rtsp_output_session::setup_callback;
    handler.onplay = &rtsp_output_session::play_callback;
    handler.onpause = &rtsp_output_session::pause_callback;
    handler.onteardown = &rtsp_output_session::teardown_callback;
    handler.onoptions = &rtsp_output_session::options_callback;
    handler.ongetparameter = &rtsp_output_session::get_parameter_callback;
    handler.onsetparameter = &rtsp_output_session::set_parameter_callback;

    const auto ip = local_ip(*connection_);
    server_ = rtsp_server_create(ip.c_str(), server_port_, &handler, this, this);
    if (server_ == nullptr)
    {
        close();
        return;
    }

    const auto self = shared_from_this();
    connection_->start(
        [self](std::span<const std::uint8_t> data) { self->on_read(data); },
        [self]() { self->on_close(); });
}

void rtsp_output_session::on_track(const media_track& track)
{
    const auto iterator = tracks_.find(track.id);
    if (iterator != tracks_.end() && iterator->second.track.config_version != track.config_version)
    {
        // RTSP SDP 已经发给客户端，配置改变时第一阶段直接结束旧会话。
        close();
    }
}

void rtsp_output_session::on_frame(const media_frame& frame)
{
    const auto iterator = tracks_.find(frame.track);
    if (iterator == tracks_.end() || !frame.payload || muxer_ == nullptr)
    {
        return;
    }
    const auto& state = iterator->second;
    if (!state.setup || state.media_id < 0)
    {
        return;
    }

    const auto result = rtsp_muxer_input(
        muxer_,
        state.media_id,
        ns_to_milliseconds(frame.pts_ns),
        ns_to_milliseconds(frame.dts_ns),
        frame.payload->data(),
        static_cast<int>(frame.payload->size()),
        frame.key_frame ? 1 : 0);
    if (result < 0)
    {
        log_line("rtsp_output", "mux failed", result);
    }
}

void rtsp_output_session::on_end()
{
    close();
}

int rtsp_output_session::close_callback(void*)
{
    return 0;
}

int rtsp_output_session::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_output_session*>(param);
    self->connection_->write(data, bytes);
    return 0;
}

int rtsp_output_session::describe_callback(void* param, rtsp_server_t*, const char* uri)
{
    return static_cast<rtsp_output_session*>(param)->on_describe(uri != nullptr ? uri : "");
}

int rtsp_output_session::setup_callback(
    void* param,
    rtsp_server_t*,
    const char* uri,
    const char* session,
    const rtsp_header_transport_t transports[],
    std::size_t count)
    {
    return static_cast<rtsp_output_session*>(param)->on_setup(
        uri != nullptr ? uri : "",
        session != nullptr ? session : "",
        transports,
        count);
}

int rtsp_output_session::play_callback(
    void* param,
    rtsp_server_t*,
    const char*,
    const char* session,
    const std::int64_t* npt,
    const double*)
    {
    return static_cast<rtsp_output_session*>(param)->on_play(session != nullptr ? session : "", npt);
}

int rtsp_output_session::pause_callback(
    void*, rtsp_server_t* server, const char*, const char*, const std::int64_t*)
    {
    return rtsp_server_reply_pause(server, 460);
}

int rtsp_output_session::teardown_callback(
    void* param, rtsp_server_t* server, const char*, const char*)
    {
    const auto result = rtsp_server_reply_teardown(server, 200);
    auto* self = static_cast<rtsp_output_session*>(param);
    const auto keep_alive = self->shared_from_this();
    boost::asio::post(
        self->connection_->socket().get_executor(),
        [keep_alive]() { keep_alive->close(); });
    return result;
}

int rtsp_output_session::options_callback(void*, rtsp_server_t* server, const char*)
{
    return rtsp_server_reply_options(server, 200);
}

int rtsp_output_session::get_parameter_callback(
    void*, rtsp_server_t* server, const char*, const char*, const void*, int)
    {
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

int rtsp_output_session::set_parameter_callback(
    void*, rtsp_server_t* server, const char*, const char*, const void*, int)
    {
    return rtsp_server_reply_set_parameter(server, 200);
}

int rtsp_output_session::muxer_packet_callback(
    void* param,
    int pid,
    const void* data,
    int bytes,
    std::uint32_t,
    int)
    {
    return static_cast<rtsp_output_session*>(param)->on_muxer_packet(pid, data, bytes);
}

void rtsp_output_session::on_read(std::span<const std::uint8_t> data)
{
    if (server_ == nullptr || closed_)
    {
        return;
    }

    auto remaining = data;
    while (!remaining.empty() && !closed_)
    {
        auto bytes = remaining.size();
        const auto result = rtsp_server_input(server_, remaining.data(), &bytes);
        if (result != 0 && result != 1)
        {
            close();
            return;
        }
        if (result == 1 || bytes == 0)
        {
            return;
        }
        const auto consumed = remaining.size() - bytes;
        if (consumed == 0)
        {
            close();
            return;
        }
        remaining = remaining.subspan(consumed);
    }
}

void rtsp_output_session::on_close()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_ && playing_)
    {
        stream_->remove_sink(this);
    }
    stream_.reset();
    log_line("rtsp_output", "close", stream_name_);
}

void rtsp_output_session::close()
{
    if (!closed_)
    {
        connection_->close();
    }
}

int rtsp_output_session::on_describe(std::string_view uri)
{
    stream_name_ = stream_name_from_uri(uri);
    stream_ = registry_.find(stream_name_);
    if (!stream_)
    {
        return rtsp_server_reply_describe(server_, 404, "");
    }

    if (muxer_ != nullptr)

    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
    muxer_ = rtsp_muxer_create(&rtsp_output_session::muxer_packet_callback, this);
    tracks_.clear();

    std::ostringstream sdp;
    const auto ip = local_ip(*connection_);
    sdp << "v=0\r\n"
        << "o=- 1 1 IN IP4 " << ip << "\r\n"
        << "s=media_server\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n";

    const auto snapshot = stream_->tracks();
    std::string media_sdp;
    if (!configure_tracks(snapshot, media_sdp))
    {
        return rtsp_server_reply_describe(server_, 415, "");
    }
    sdp << media_sdp;
    log_line("rtsp_output", "describe", stream_name_);
    return rtsp_server_reply_describe(server_, 200, sdp.str().c_str());
}

int rtsp_output_session::on_setup(
    std::string_view uri,
    std::string_view session,
    const rtsp_header_transport_t transports[],
    std::size_t count)
    {

    const auto id = track_id_from_uri(uri);
    if (!id)
    {
        return rtsp_server_reply_setup(server_, 404, nullptr, nullptr);
    }
    auto iterator = tracks_.find(*id);
    if (iterator == tracks_.end())
    {
        return rtsp_server_reply_setup(server_, 404, nullptr, nullptr);
    }

    const rtsp_header_transport_t* selected = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (transports[index].transport == RTSP_TRANSPORT_RTP_TCP)
        {
            selected = &transports[index];
            break;
        }
    }
    if (selected == nullptr)
    {
        return rtsp_server_reply_setup(server_, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    if (session_id_.empty())

    {
        session_id_ = session.empty() ? std::to_string(reinterpret_cast<std::uintptr_t>(this)) : std::string(session);
    }
    iterator->second.rtp_channel = static_cast<std::uint8_t>(selected->interleaved1);
    iterator->second.rtcp_channel = static_cast<std::uint8_t>(selected->interleaved2);
    iterator->second.setup = true;

    std::ostringstream transport;
    transport << "RTP/AVP/TCP;unicast;interleaved="
              << selected->interleaved1 << '-' << selected->interleaved2;
    return rtsp_server_reply_setup(server_, 200, session_id_.c_str(), transport.str().c_str());
}

int rtsp_output_session::on_play(std::string_view session, const std::int64_t* npt)
{
    if (session_id_.empty() || session != session_id_ || !stream_)
    {
        return rtsp_server_reply_play(server_, 454, nullptr, nullptr, nullptr);
    }

    if (!playing_)

    {
        playing_ = true;
        if (!stream_->add_sink(shared_from_this()))
        {
            return rtsp_server_reply_play(server_, 454, nullptr, nullptr, nullptr);
        }
    }
    return rtsp_server_reply_play(server_, 200, npt, nullptr, nullptr);
}

int rtsp_output_session::on_muxer_packet(int pid, const void* data, int bytes)
{
    if (data == nullptr || bytes <= 0)
    {
        return 0;
    }

    auto iterator = std::find_if(
        tracks_.begin(), tracks_.end(),
        [pid](const auto& item) { return item.second.payload_index == pid; });
    if (iterator == tracks_.end() || !iterator->second.setup)
    {
        return 0;
    }

    std::array<std::uint8_t, 4> header{};
    header[0] = 0x24;
    header[1] = iterator->second.rtp_channel;
    const auto network_bytes = htons(static_cast<std::uint16_t>(bytes));
    std::memcpy(header.data() + 2, &network_bytes, sizeof(network_bytes));
    connection_->write(header);
    connection_->write(data, static_cast<std::size_t>(bytes));

    std::array<std::uint8_t, 1500> rtcp{};
    const auto rtcp_bytes = rtsp_muxer_rtcp(muxer_, pid, rtcp.data(), static_cast<int>(rtcp.size()));
    if (rtcp_bytes > 0)
    {
        header[1] = iterator->second.rtcp_channel;
        const auto network_rtcp_bytes = htons(static_cast<std::uint16_t>(rtcp_bytes));
        std::memcpy(header.data() + 2, &network_rtcp_bytes, sizeof(network_rtcp_bytes));
        connection_->write(header);
        connection_->write(rtcp.data(), static_cast<std::size_t>(rtcp_bytes));
    }
    return 0;
}

bool rtsp_output_session::configure_tracks(
    std::span<const media_track> tracks,
    std::string& sdp)
    {

    if (muxer_ == nullptr)

    {
        return false;
    }
    int next_payload_type = 96;
    std::ostringstream output;

    for (const auto& track : tracks)

    {
        std::vector<std::uint8_t> extra;
        const char* encoding = nullptr;
        int rtp_codec = -1;
        int frequency = static_cast<int>(track.clock_rate);

        if (track.codec == codec_id::h264)

        {
            extra = h264_annex_b_to_avcc(track.codec_config);
            if (extra.empty())
            {
                continue;
            }
            encoding = "H264";
            rtp_codec = RTP_PAYLOAD_H264;
            frequency = 90'000;
        } else if (track.codec == codec_id::aac)
        {
            extra = track.codec_config;
            if (extra.empty() || track.clock_rate == 0)
            {
                continue;
            }
            encoding = "MPEG4-GENERIC";
            rtp_codec = RTP_PAYLOAD_MP4A;
        }
        else
        {
            continue;
        }

        const auto payload_type = next_payload_type++;
        const auto payload_index = rtsp_muxer_add_payload(
            muxer_,
            "RTP/AVP",
            frequency,
            payload_type,
            encoding,
            0,
            random_u32(),
            0,
            extra.data(),
            static_cast<int>(extra.size()));
        if (payload_index < 0)
        {
            return false;
        }
        const auto media_id = rtsp_muxer_add_media(
            muxer_, payload_index, rtp_codec, extra.data(), static_cast<int>(extra.size()));
        if (media_id < 0)
        {
            return false;
        }

        std::uint16_t sequence{};
        std::uint32_t timestamp{};
        const char* media_text{};
        int media_text_size{};
        if (rtsp_muxer_getinfo(
                muxer_, payload_index, &sequence, &timestamp, &media_text, &media_text_size) != 0)
                {
            return false;
        }
        output.write(media_text, media_text_size);
        output << "a=control:trackID=" << track.id << "\r\n";

        tracks_.insert_or_assign(
            track.id,
            track_state{
                .track = track,
                .payload_index = payload_index,
                .media_id = media_id,
                .payload_type = payload_type,
            });
    }

    sdp = output.str();
    return !tracks_.empty();
}

std::string rtsp_output_session::stream_name_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return {};
    }

    std::string result;
    for (const auto segment : parsed->segments())
    {
        const std::string value(segment);
        if (value.starts_with("trackID="))
        {
            break;
        }
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(value);
    }
    return result;
}

std::optional<track_id> rtsp_output_session::track_id_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return std::nullopt;
    }

    for (const auto segment : parsed->segments())
    {
        const std::string_view value(segment.data(), segment.size());
        constexpr std::string_view prefix = "trackID=";
        if (!value.starts_with(prefix))
        {
            continue;
        }

        const auto text = value.substr(prefix.size());
        track_id id = 0;
        const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error != std::errc{} || pointer != text.data() + text.size())
        {
            return std::nullopt;
        }
        return id;
    }
    return std::nullopt;
}

}    // namespace media_server
