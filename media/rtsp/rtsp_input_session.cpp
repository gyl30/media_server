#include <algorithm>
#include <random>
#include <string>
#include <utility>

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_sdp.h"
#include "media/rtsp/rtsp_uri.h"
#include "media/rtsp/rtsp_input_tcp_session.h"
#include "media/rtsp/rtsp_input_udp_session.h"

extern "C"
{
#include "rtp-profile.h"
}

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

}    // namespace

rtsp_input_session::rtsp_input_session(boost::asio::any_io_executor executor,
                                       std::function<void(std::span<const std::uint8_t>)> write)
    : executor_(std::move(executor)), write_(std::move(write))
{
}

void rtsp_input_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (!tcp_session_)
    {
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
        return;
    }
    tcp_session_->on_interleaved(channel, data);
}

int rtsp_input_session::on_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length)
{
    if (!session_id_.empty() || sdp == nullptr || length <= 0)
    {
        return rtsp_server_reply_announce(server, 455);
    }

    descriptions_.clear();
    stream_name_ = rtsp_path_from_uri(uri);
    if (stream_name_.empty())
    {
        return rtsp_server_reply_announce(server, 400);
    }

    const auto count = rtsp_media_sdp(sdp, length, nullptr, 0);
    if (count <= 0)
    {
        return rtsp_server_reply_announce(server, 415);
    }
    std::vector<rtsp_media_t> media(static_cast<std::size_t>(count));
    if (rtsp_media_sdp(sdp, length, media.data(), count) != count)
    {
        return rtsp_server_reply_announce(server, 415);
    }

    const auto* content_base = rtsp_server_get_header(server, "Content-Base");
    const auto* content_location = rtsp_server_get_header(server, "Content-Location");
    for (auto& description : media)
    {
        if (rtsp_media_set_url(&description, content_base, content_location, std::string(uri).c_str()) != 0)
        {
            return rtsp_server_reply_announce(server, 400);
        }
    }

    bool video = false;
    bool audio = false;
    for (const auto& description : media)
    {
        std::optional<rtsp_input_track_description> selected;
        for (int format_index = 0; format_index < description.avformat_count; ++format_index)
        {
            auto format = description.avformats[static_cast<std::size_t>(format_index)];
            if (format.rate == 0)
            {
                if (const auto* profile = rtp_profile_find(format.fmt))
                {
                    format.rate = profile->frequency;
                }
            }
            const auto id = rtsp_sdp_iequals(description.media, "video") ? video_track_id : audio_track_id;
            auto track = rtsp_sdp_track_from_format(description.media, format.fmt, format.rate, format.encoding, format.fmtp, id);
            if (!track)
            {
                continue;
            }
            selected = rtsp_input_track_description{
                .uri = description.uri,
                .track = std::move(*track),
                .clock_rate = format.rate,
                .payload_type = format.fmt,
                .encoding = format.encoding,
                .fmtp = format.fmtp != nullptr ? format.fmtp : "",
            };
            break;
        }
        if (!selected)
        {
            continue;
        }
        if ((selected->track.kind == media_kind::video && video) || (selected->track.kind == media_kind::audio && audio) || descriptions_.size() >= 2)
        {
            descriptions_.clear();
            return rtsp_server_reply_announce(server, 415);
        }
        if (selected->track.kind == media_kind::video)
        {
            video = true;
        }
        else
        {
            audio = true;
        }
        descriptions_.push_back(std::move(*selected));
    }
    if (!video)
    {
        descriptions_.clear();
        return rtsp_server_reply_announce(server, 415);
    }

    session_id_ = std::to_string(random_u32());
    return rtsp_server_reply_announce(server, 200);
}

int rtsp_input_session::on_setup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (session_id_.empty() || (!session.empty() && session != session_id_))
    {
        return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
    }

    const auto description =
        std::ranges::find_if(descriptions_, [uri](const rtsp_input_track_description& value) { return uri == value.uri; });
    if (description == descriptions_.end())
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto track_index = static_cast<std::size_t>(description - descriptions_.begin());

    const rtsp_header_transport_t* selected = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        const bool tcp = transports[index].transport == RTSP_TRANSPORT_RTP_TCP;
        const bool udp = transports[index].transport == RTSP_TRANSPORT_RTP_UDP;
        const bool valid_interleaved = transports[index].interleaved1 >= 0 && transports[index].interleaved2 >= 0 &&
                                       transports[index].interleaved1 <= 255 && transports[index].interleaved2 <= 255 &&
                                       transports[index].interleaved1 != transports[index].interleaved2;
        const bool family_matches = (tcp && !udp_session_) || (udp && !tcp_session_);
        if (family_matches && transports[index].multicast == 0 &&
            (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_RECORD) && (!tcp || valid_interleaved))
        {
            selected = &transports[index];
            break;
        }
    }
    if (selected == nullptr)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, udp_session_ ? nullptr : "RTP/AVP/TCP;unicast;interleaved=0-1");
    }
    if (selected->transport == RTSP_TRANSPORT_RTP_UDP && (selected->rtp.u.client_port1 == 0 || selected->rtp.u.client_port2 == 0))
    {
        return rtsp_server_reply_setup(server, 461, nullptr, nullptr);
    }

    if (tcp_session_)
    {
        return tcp_session_->on_setup(server, track_index, *selected, session_id_);
    }
    if (udp_session_)
    {
        return udp_session_->on_setup(server, track_index, *selected, session_id_);
    }

    if (selected->transport == RTSP_TRANSPORT_RTP_TCP)
    {
        auto child = std::make_shared<rtsp_input_tcp_session>(executor_, stream_name_, descriptions_, write_);
        child->set_error_handle(error_handle_);
        const auto result = child->startup(server, track_index, *selected, session_id_);
        if (!child->closed_)
        {
            tcp_session_ = std::move(child);
            write_ = {};
            stream_name_.clear();
        }
        return result;
    }

    auto child = std::make_shared<rtsp_input_udp_session>(executor_, stream_name_, descriptions_);
    child->set_error_handle(error_handle_);
    const auto result = child->startup(server, track_index, *selected, session_id_);
    if (!child->closed_)
    {
        udp_session_ = std::move(child);
        write_ = {};
        stream_name_.clear();
    }
    return result;
}

int rtsp_input_session::on_record(rtsp_server_t* server,
                                  std::string_view,
                                  std::string_view session,
                                  const std::int64_t*,
                                  const double*)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (tcp_session_)
    {
        return tcp_session_->on_record(server);
    }
    if (udp_session_)
    {
        return udp_session_->on_record(server);
    }
    return rtsp_server_reply_record(server, 455, nullptr, nullptr);
}

int rtsp_input_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    error_handle_(boost::asio::error::eof);
    return result;
}

void rtsp_input_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (tcp_session_)
    {
        tcp_session_->safe_shutdown();
        tcp_session_.reset();
    }
    if (udp_session_)
    {
        udp_session_->safe_shutdown();
        udp_session_.reset();
    }
    write_ = {};
    error_handle_ = {};
}

}    // namespace media_server
