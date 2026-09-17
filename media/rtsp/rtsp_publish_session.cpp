#include <random>
#include <string>
#include <utility>
#include <algorithm>

#include "media/rtsp/rtsp_sdp.h"
#include "media/rtsp/rtsp_uri.h"
#include "media/rtsp/rtsp_event.h"
#include "media/http/signaling_client.h"
#include "media/rtsp/rtsp_publish_session.h"
#include "media/rtsp/rtsp_publish_tcp_session.h"
#include "media/rtsp/rtsp_publish_udp_session.h"

extern "C"
{
#include "rtsp-media.h"
#include "rtp-profile.h"
#include "rtsp-server.h"
#include "rtsp-header-transport.h"
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

rtsp_publish_session::rtsp_publish_session(worker_context& worker,
                                           boost::asio::ip::address bind_address,
                                           std::function<void(std::span<const std::uint8_t>)> write,
                                           std::chrono::milliseconds rtcp_interval)
    : worker_(worker), bind_address_(std::move(bind_address)), rtcp_interval_(rtcp_interval), write_handler_(std::move(write))
{
}

bool rtsp_publish_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (!tcp_session_)
    {
        return false;
    }
    const auto result = tcp_session_->on_interleaved(channel, data);
    if (!result && notify_shutdown())
    {
        signaling_client::instance().report(
            rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::protocol_error, "media", "media_input_failed"));
    }
    return result;
}

int rtsp_publish_session::prepare_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length)
{
    if (announce_prepared_ || !session_id_.empty() || sdp == nullptr || length <= 0)
    {
        return 455;
    }

    const auto target = parse_rtsp_publish_target(uri);
    if (!target)
    {
        return 400;
    }

    const auto count = rtsp_media_sdp(sdp, length, nullptr, 0);
    if (count <= 0)
    {
        return 415;
    }
    std::vector<rtsp_media_t> media(static_cast<std::size_t>(count));
    if (rtsp_media_sdp(sdp, length, media.data(), count) != count)
    {
        return 415;
    }

    const auto* content_base = rtsp_server_get_header(server, "Content-Base");
    const auto* content_location = rtsp_server_get_header(server, "Content-Location");
    const std::string uri_value(uri);
    for (auto& description : media)
    {
        if (rtsp_media_set_url(&description, content_base, content_location, uri_value.c_str()) != 0)
        {
            return 400;
        }
    }

    std::vector<rtsp_publish_track_description> descriptions;
    bool video = false;
    bool audio = false;
    for (const auto& description : media)
    {
        std::optional<rtsp_publish_track_description> selected;
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
            selected = rtsp_publish_track_description{
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
        if ((selected->track.kind == media_kind::video && video) || (selected->track.kind == media_kind::audio && audio) || descriptions.size() >= 2)
        {
            return 415;
        }
        if (selected->track.kind == media_kind::video)
        {
            video = true;
        }
        else
        {
            audio = true;
        }
        descriptions.push_back(std::move(*selected));
    }
    if (!video)
    {
        return 415;
    }

    stream_id_ = target->stream_id;
    stream_name_ = target->stream_name;
    descriptions_ = std::move(descriptions);
    announce_prepared_ = true;
    return 200;
}

int rtsp_publish_session::accept_announce(rtsp_server_t* server)
{
    if (!announce_prepared_ || !session_id_.empty())
    {
        return rtsp_server_reply_announce(server, 455);
    }

    announce_prepared_ = false;
    session_id_ = std::to_string(random_u32());
    return rtsp_server_reply_announce(server, 200);
}

int rtsp_publish_session::on_setup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (session_id_.empty() || (!session.empty() && session != session_id_))
    {
        return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
    }

    const auto description = std::ranges::find_if(descriptions_, [uri](const rtsp_publish_track_description& value) { return uri == value.uri; });
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
        if (family_matches && transports[index].multicast == 0 && (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_RECORD) &&
            (!tcp || valid_interleaved))
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
        const auto result = tcp_session_->on_setup(server, track_index, *selected, session_id_);
        if (result < 0 && notify_shutdown())
        {
            signaling_client::instance().report(
                rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::runtime_error, "setup", "setup_failed"));
        }
        return result;
    }
    if (udp_session_)
    {
        const auto result = udp_session_->on_setup(server, track_index, *selected, session_id_);
        if (result < 0 && notify_shutdown())
        {
            signaling_client::instance().report(
                rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::runtime_error, "setup", "setup_failed"));
        }
        return result;
    }

    if (selected->transport == RTSP_TRANSPORT_RTP_TCP)
    {
        auto child = std::make_shared<rtsp_publish_tcp_session>(worker_, stream_name_, descriptions_, write_handler_);
        child->media_.set_streaming_handler([this]() { notify_streaming_if_ready(); });
        const auto result = child->startup(server, track_index, *selected, session_id_);
        tcp_session_ = std::move(child);
        write_handler_ = {};
        if (result < 0 && notify_shutdown())
        {
            signaling_client::instance().report(rtsp_event::publisher_stopped(
                stream_id_, stream_name_, runtime_end_reason::runtime_error, "setup", "publish_transport_startup_failed"));
        }
        return result;
    }

    auto child = std::make_shared<rtsp_publish_udp_session>(worker_, bind_address_, stream_name_, descriptions_, rtcp_interval_);
    child->media_.set_streaming_handler([this]() { notify_streaming_if_ready(); });
    child->set_shutdown_handler(
        [this]()
        {
            if (!notify_shutdown())
            {
                return;
            }
            if (udp_session_ && udp_session_->media_.protocol_error())
            {
                signaling_client::instance().report(
                    rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::protocol_error, "media", "media_input_failed"));
                return;
            }
            signaling_client::instance().report(rtsp_event::publisher_stopped(
                stream_id_, stream_name_, runtime_end_reason::runtime_error, "transport", "udp_media_transport_failed"));
        });
    const auto result = child->startup(server, track_index, *selected, session_id_);
    udp_session_ = std::move(child);
    write_handler_ = {};
    if (result < 0 && notify_shutdown())
    {
        signaling_client::instance().report(
            rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::runtime_error, "setup", "publish_transport_startup_failed"));
    }
    return result;
}

int rtsp_publish_session::on_record(rtsp_server_t* server, std::string_view, std::string_view session, const std::int64_t*, const double*)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    if (tcp_session_)
    {
        const auto result = tcp_session_->on_record(server);
        notify_streaming_if_ready();
        if (result < 0 && notify_shutdown())
        {
            signaling_client::instance().report(
                rtsp_event::publisher_stopped(stream_id_,
                                              stream_name_,
                                              runtime_end_reason::runtime_error,
                                              tcp_session_->media_.recording() ? "control" : "media",
                                              tcp_session_->media_.recording() ? "record_reply_failed" : "stream_registry_add_failed"));
        }
        return result;
    }
    if (udp_session_)
    {
        const auto result = udp_session_->on_record(server);
        notify_streaming_if_ready();
        if (result < 0 && notify_shutdown())
        {
            signaling_client::instance().report(
                rtsp_event::publisher_stopped(stream_id_,
                                              stream_name_,
                                              runtime_end_reason::runtime_error,
                                              udp_session_->media_.recording() ? "control" : "media",
                                              udp_session_->media_.recording() ? "record_reply_failed" : "stream_registry_add_failed"));
        }
        return result;
    }
    return rtsp_server_reply_record(server, 455, nullptr, nullptr);
}

int rtsp_publish_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    if (result == 0)
    {
        if (notify_shutdown())
        {
            signaling_client::instance().report(rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::remote, "control"));
        }
        return -1;
    }
    if (notify_shutdown())
    {
        signaling_client::instance().report(
            rtsp_event::publisher_stopped(stream_id_, stream_name_, runtime_end_reason::runtime_error, "control", "teardown_reply_failed"));
    }
    return result;
}

void rtsp_publish_session::shutdown()
{
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
    write_handler_ = {};
    shutdown_handler_ = {};
    streaming_handler_ = {};
}

bool rtsp_publish_session::notify_shutdown()
{
    if (shutdown_notified_)
    {
        return false;
    }
    shutdown_notified_ = true;
    if (shutdown_handler_)
    {
        std::move(shutdown_handler_)();
    }
    return true;
}

void rtsp_publish_session::notify_streaming_if_ready()
{
    if (streaming_notified_ || shutdown_notified_)
    {
        return;
    }
    const bool recording = (tcp_session_ && tcp_session_->media_.recording()) || (udp_session_ && udp_session_->media_.recording());
    if (!recording)
    {
        return;
    }
    streaming_notified_ = true;
    if (streaming_handler_)
    {
        streaming_handler_();
    }
}

}    // namespace media_server
