#include <vector>
#include <cstddef>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/webrtc/stun_message.h"
#include "media/webrtc/whep_session.h"
#include "media/webrtc/whep_audio_egress.h"

namespace media_server
{
namespace
{

constexpr auto establishment_timeout = std::chrono::seconds{15};
constexpr auto ice_activity_timeout = std::chrono::seconds{30};

}    // namespace

whep_session::whep_session(worker_context& worker, std::shared_ptr<media_stream> stream,
                           std::shared_ptr<udp_transport> transport, webrtc_answer_config config)
    : worker_(worker),
      stream_(std::move(stream)),
      udp_transport_(std::move(transport)),
      dtls_timer_(worker_.io()),
      establishment_timer_(worker_.io()),
      ice_activity_timer_(worker_.io()),
      local_port_(config.port),
      id_(std::move(config.stream_id)),
      ice_ufrag_(std::move(config.ice_ufrag)),
      ice_pwd_(std::move(config.ice_pwd))
{
}

bool whep_session::startup(const webrtc_media_offer& transport_offer,
                           const webrtc_answer& answer, const dtls_certificate& certificate)
{
    const auto self = shared_from_this();
    udp_transport_->set_write_callback(
        [weak = weak_from_this()](boost::system::error_code error, std::size_t)
        {
            if (!error)
            {
                return;
            }
            const auto locked = weak.lock();
            if (!locked || locked->local_port_ == 0)
            {
                return;
            }
            if (error != boost::asio::error::operation_aborted)
            {
                spdlog::debug("webrtc udp send failed session {} error {}", locked->id_, error.message());
            }
            locked->shutdown();
        });

    remote_ice_ufrag_ = transport_offer.ice_ufrag;
    dtls_ = std::make_unique<dtls_transport>(transport_offer.fingerprint,
                                             [self](std::span<const std::uint8_t> packet)
                                             { self->send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end())); });
    if (!dtls_->startup(certificate))
    {
        spdlog::error("webrtc dtls transport startup failed session {}", id_);
        return false;
    }

    spdlog::debug("webrtc session {} remote fingerprint {}", id_, transport_offer.fingerprint);

    if (answer.audio_payload_type && answer.audio_codec == codec_id::aac)
    {
        audio_egress_ = acquire_whep_audio_egress(stream_,
                                                  worker_,
                                                  whep_audio_settings{
                                                      .channels = answer.audio_channel_count.value_or(1),
                                                      .bitrate = answer.audio_bitrate.value_or(64'000 * answer.audio_channel_count.value_or(1)),
                                                      .max_playback_rate = answer.audio_max_playback_rate.value_or(48'000),
                                                  });
        if (!audio_egress_)
        {
            return false;
        }
        stream_ = audio_egress_->output_stream();
    }

    packetizer_ = std::make_unique<webrtc_packetizer>(
        [self](std::span<const std::uint8_t> packet) { return self->send_rtp(packet); },
        [self](std::span<const std::uint8_t> packet) { return self->send_rtcp(packet); });
    if (!packetizer_->startup(
        stream_->tracks(),
        webrtc_packetizer_config{
            .video_codec = answer.video_codec.value_or(codec_id::h264),
            .audio_codec = audio_egress_ ? codec_id::opus : answer.audio_codec.value_or(codec_id::aac),
            .video_payload_type = answer.video_payload_type.value_or(-1),
            .audio_payload_type = answer.audio_payload_type.value_or(-1),
            .video_mid = answer.video_mid.value_or(""),
            .audio_mid = answer.audio_mid.value_or(""),
            .video_mid_extension_id = answer.video_mid_extension_id.value_or(-1),
            .audio_mid_extension_id = answer.audio_mid_extension_id.value_or(-1),
            .rtcp_cname = id_,
        }))
    {
        return false;
    }

    for (const auto& track : stream_->tracks())
    {
        if (answer.video_codec && track.kind == media_kind::video && track.codec == *answer.video_codec)
        {
            waiting_video_track_ = track.id;
            break;
        }
    }
    spdlog::debug(
        "webrtc session {} local_ufrag {} remote_ufrag {} video_pt {} audio_pt {} audio_channels {} audio_bitrate {} audio_max_playback_rate {}",
        id_,
        ice_ufrag_,
        remote_ice_ufrag_,
        answer.video_payload_type.value_or(-1),
        answer.audio_payload_type.value_or(-1),
        answer.audio_channel_count.value_or(0),
        answer.audio_bitrate.value_or(0),
        answer.audio_max_playback_rate.value_or(0));
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_udp(yield); });
    stream_->add_sink(self);
    startup_establishment_timeout();
    spdlog::info("webrtc whep session started {} stream {}", id_, stream_->stream_id());
    return true;
}

void whep_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void whep_session::safe_shutdown()
{
    if (!stream_)
    {
        return;
    }
    stream_->remove_sink(this);
    dtls_.reset();
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    packetizer_.reset();
    waiting_video_track_.reset();
    stream_.reset();
    release_whep_audio_egress(audio_egress_);
    srtp_.reset();
    dtls_timer_.cancel();
    establishment_timer_.cancel();
    ice_activity_timer_.cancel();
    udp_transport_->shutdown();
    if (local_port_ != 0)
    {
        media_port_pool::instance().release(local_port_);
        local_port_ = 0;
    }

    spdlog::info("webrtc whep session shutdown {}", id_);
}

const std::string& whep_session::id() const noexcept { return id_; }

void whep_session::on_frame(const media_frame& frame)
{
    if (!srtp_)
    {
        return;
    }
    if (waiting_video_track_)
    {
        if (frame.track != *waiting_video_track_ || !frame.key_frame)
        {
            return;
        }
        waiting_video_track_.reset();
    }
    if (!packetizer_->on_frame(frame))
    {
        shutdown();
    }
}

void whep_session::on_end()
{
    if (!stream_)
    {
        return;
    }
    spdlog::info("webrtc source stream ended session {}", id_);
    shutdown();
}

void whep_session::run_udp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    boost::system::error_code read_error;
    for (;;)
    {
        const auto bytes = udp_transport_->read(buffer, endpoint, yield, read_error);
        if (read_error)
        {
            if (read_error != boost::asio::error::operation_aborted)
            {
                spdlog::debug("webrtc udp receive failed session {} error {}", id_, read_error.message());
            }
            break;
        }
        handle_packet(std::span<const std::uint8_t>{buffer.data(), bytes}, endpoint);
    }
    shutdown();
}

void whep_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (local_port_ == 0 || packet.empty())
    {
        return;
    }

    if (is_stun_message(packet))
    {
        handle_stun(packet, endpoint);
        return;
    }

    if (!remote_endpoint_.has_value() || endpoint != *remote_endpoint_)
    {
        spdlog::trace("webrtc udp packet dropped before ice nomination session {} remote {} {} size {}",
                      id_,
                      endpoint.address().to_string(),
                      endpoint.port(),
                      packet.size());
        return;
    }

    if (dtls_transport::is_dtls_packet(packet))
    {
        spdlog::trace("webrtc dtls packet received session {} size {}", id_, packet.size());
        handle_dtls(packet);
        return;
    }

    if (srtp_transport::is_rtp_or_rtcp(packet))
    {
        spdlog::trace("webrtc inbound srtp ignored session {} size {}", id_, packet.size());
        return;
    }

    spdlog::trace("webrtc unknown udp packet session {} size {} first_byte {}", id_, packet.size(), packet.front());
}

void whep_session::handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    const auto remote_address = endpoint.address().to_string();
    const auto remote_port = endpoint.port();
    spdlog::debug("webrtc stun received session {} remote {} {} size {}", id_, remote_address, remote_port, packet.size());

    const auto username = ice_ufrag_ + ":" + remote_ice_ufrag_;
    const auto request = parse_stun_binding_request(packet, username, ice_pwd_);
    if (!request)
    {
        spdlog::debug("webrtc stun rejected session {} remote {} {}", id_, remote_address, remote_port);
        return;
    }

    if (!request->unknown_required_attributes.empty())
    {
        auto response = make_stun_binding_error_response(*request, 420, "Unknown Attribute", request->unknown_required_attributes, ice_pwd_);
        if (response.empty())
        {
            spdlog::error("webrtc stun response create failed session {}", id_);
            return;
        }
        spdlog::debug("webrtc stun unknown attribute session {} remote {} {} count {}",
                      id_,
                      remote_address,
                      remote_port,
                      request->unknown_required_attributes.size());
        send_udp(std::move(response), endpoint);
        return;
    }

    if (!request->priority)
    {
        spdlog::debug("webrtc stun rejected missing priority session {} remote {} {}", id_, remote_address, remote_port);
        return;
    }
    if (request->ice_controlled)
    {
        auto response = make_stun_binding_error_response(*request, 487, "Role Conflict", {}, ice_pwd_);
        if (response.empty())
        {
            spdlog::error("webrtc stun response create failed session {}", id_);
            return;
        }
        spdlog::debug("webrtc stun role conflict session {} remote {} {}", id_, remote_address, remote_port);
        send_udp(std::move(response), endpoint);
        return;
    }
    if (!request->ice_controlling)
    {
        spdlog::debug("webrtc stun rejected missing ice controlling session {} remote {} {}", id_, remote_address, remote_port);
        return;
    }

    spdlog::debug("webrtc stun valid session {} remote {} {} use_candidate {}", id_, remote_address, remote_port, request->use_candidate);

    auto response = make_stun_binding_success_response(*request, endpoint, ice_pwd_);
    if (response.empty())
    {
        spdlog::error("webrtc stun response create failed session {}", id_);
        return;
    }

    if (request->use_candidate)
    {
        const bool changed = !remote_endpoint_.has_value() || *remote_endpoint_ != endpoint;
        remote_endpoint_ = endpoint;
        refresh_ice_activity_timeout();
        if (changed)
        {
            spdlog::info("webrtc ice connected session {} remote {} {}", id_, remote_address, remote_port);
        }
    }
    else if (remote_endpoint_.has_value() && endpoint == *remote_endpoint_)
    {
        refresh_ice_activity_timeout();
    }

    spdlog::trace("webrtc stun response send session {} remote {} {} size {}", id_, remote_address, remote_port, response.size());
    send_udp(std::move(response), endpoint);
}

void whep_session::handle_dtls(std::span<const std::uint8_t> packet)
{
    const bool was_connected = dtls_->connected();
    if (!dtls_->handle_datagram(packet))
    {
        shutdown();
        return;
    }

    if (!was_connected && dtls_->connected())
    {
        dtls_timer_.cancel();
        spdlog::info("webrtc dtls connected session {}", id_);
        if (!startup_media())
        {
            spdlog::error("webrtc srtp startup failed session {}", id_);
            shutdown();
        }
        return;
    }

    schedule_dtls_timeout();
}

bool whep_session::startup_media()
{
    const auto& keying_material = *dtls_->srtp_keying_material();
    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->startup(keying_material))
    {
        return false;
    }

    srtp_ = std::move(srtp);

    establishment_timer_.cancel();
    spdlog::info("webrtc srtp started session {}", id_);
    spdlog::debug("webrtc srtp profile session {} {}", id_, keying_material.profile);
    return true;
}

int whep_session::send_rtp(std::span<const std::uint8_t> packet)
{
    spdlog::trace("webrtc rtp protect session {} plain_size {}", id_, packet.size());
    auto protected_packet = srtp_->protect_rtp(packet);
    if (!protected_packet)
    {
        spdlog::error("webrtc srtp protect failed session {}", id_);
        return -1;
    }
    spdlog::trace("webrtc rtp protected session {} protected_size {}", id_, protected_packet->size());
    send_udp(std::move(*protected_packet));
    return 0;
}

int whep_session::send_rtcp(std::span<const std::uint8_t> packet)
{
    spdlog::trace("webrtc rtcp protect session {} plain_size {}", id_, packet.size());
    auto protected_packet = srtp_->protect_rtcp(packet);
    if (!protected_packet)
    {
        spdlog::error("webrtc srtcp protect failed session {}", id_);
        return -1;
    }
    spdlog::trace("webrtc rtcp protected session {} protected_size {}", id_, protected_packet->size());
    send_udp(std::move(*protected_packet));
    return 0;
}

void whep_session::send_udp(std::vector<std::uint8_t> packet)
{
    if (!remote_endpoint_.has_value())
    {
        return;
    }
    send_udp(std::move(packet), *remote_endpoint_);
}

void whep_session::send_udp(std::vector<std::uint8_t> packet, boost::asio::ip::udp::endpoint endpoint)
{
    if (local_port_ == 0 || packet.empty())
    {
        return;
    }
    if (!udp_transport_->write(std::move(packet), std::move(endpoint)))
    {
        spdlog::warn("whep udp write queue full session {}", id_);
    }
}

void whep_session::schedule_dtls_timeout()
{
    if (!dtls_ || dtls_->connected())
    {
        return;
    }

    const auto timeout = dtls_->timeout();
    if (!timeout)
    {
        return;
    }

    spdlog::trace("webrtc dtls timeout scheduled session {} milliseconds {}", id_, timeout->count());
    dtls_timer_.expires_after(*timeout);
    const auto self = shared_from_this();
    dtls_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (!error)
            {
                self->handle_dtls_timeout();
            }
        });
}

void whep_session::handle_dtls_timeout()
{
    if (!dtls_ || dtls_->connected())
    {
        return;
    }

    spdlog::trace("webrtc dtls timeout fired session {}", id_);
    if (!dtls_->handle_timeout())
    {
        spdlog::error("webrtc dtls timeout failed session {}", id_);
        shutdown();
        return;
    }
    schedule_dtls_timeout();
}

void whep_session::startup_establishment_timeout()
{
    establishment_timer_.expires_after(establishment_timeout);
    const auto self = shared_from_this();
    establishment_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || self->local_port_ == 0 || self->srtp_)
            {
                return;
            }

            spdlog::info("webrtc establishment timeout session {}", self->id_);
            self->shutdown();
        });
}

void whep_session::refresh_ice_activity_timeout()
{
    ice_activity_timer_.expires_after(ice_activity_timeout);
    const auto self = shared_from_this();
    ice_activity_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || !self->remote_endpoint_.has_value())
            {
                return;
            }

            spdlog::info("webrtc ice activity timeout session {}", self->id_);
            self->shutdown();
        });
}

}    // namespace media_server
