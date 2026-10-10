#include <vector>
#include <cstddef>
#include <chrono>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/webrtc/stun_message.h"
#include "media/webrtc/whip_session.h"

namespace media_server
{
namespace
{

constexpr auto establishment_timeout = std::chrono::seconds{15};
constexpr auto ice_activity_timeout = std::chrono::seconds{30};

bool is_rtcp(std::span<const std::uint8_t> packet) { return packet.size() >= 2U && packet[1] >= 192U && packet[1] <= 223U; }

}    // namespace

whip_session::whip_session(worker_context& worker, std::string stream_id,
                           std::shared_ptr<udp_transport> transport, webrtc_answer_config config)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
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

bool whip_session::startup(const webrtc_media_offer& transport_offer,
                           const webrtc_answer& answer, const dtls_certificate& certificate)
{
    remote_ice_ufrag_ = transport_offer.ice_ufrag;
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
    dtls_ = std::make_unique<dtls_transport>(transport_offer.fingerprint,
                                             [self](std::span<const std::uint8_t> packet)
                                             { self->send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end())); });
    if (!dtls_->startup(certificate))
    {
        spdlog::error("webrtc dtls transport startup failed session {}", id_);
        return false;
    }

    media_receiver_ = std::make_unique<whip_media_receiver>(worker_, stream_id_);
    if (!media_receiver_->startup(whip_media_receiver_config{
            .video_codec = *answer.video_codec,
            .video_payload_type = *answer.video_payload_type,
            .audio_payload_type = answer.audio_payload_type.value_or(-1),
            .audio_channel_count = static_cast<std::uint16_t>(answer.audio_channel_count.value_or(2)),
        }))
    {
        return false;
    }
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_udp(yield); });
    startup_establishment_timeout();
    spdlog::info("webrtc whip session started {} stream {}", id_, stream_id_);
    return true;
}

void whip_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void whip_session::safe_shutdown()
{
    session_registry::instance().remove_receiver_session(stream_id_, *this);
    dtls_.reset();
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    media_receiver_.reset();
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

    spdlog::info("webrtc whip session shutdown {}", id_);
}

const std::string& whip_session::id() const noexcept { return id_; }

void whip_session::run_udp(boost::asio::yield_context yield)
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

void whip_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
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
        return;
    }

    if (dtls_transport::is_dtls_packet(packet))
    {
        handle_dtls(packet);
        return;
    }

    if (srtp_transport::is_rtp_or_rtcp(packet))
    {
        handle_srtp(packet);
    }
}

void whip_session::handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    const auto remote_address = endpoint.address().to_string();
    const auto remote_port = endpoint.port();
    const auto username = ice_ufrag_ + ":" + remote_ice_ufrag_;
    const auto request = parse_stun_binding_request(packet, username, ice_pwd_);
    if (!request)
    {
        return;
    }

    if (!request->unknown_required_attributes.empty())
    {
        auto response = make_stun_binding_error_response(*request, 420, "Unknown Attribute", request->unknown_required_attributes, ice_pwd_);
        if (!response.empty())
        {
            send_udp(std::move(response), endpoint);
        }
        return;
    }
    if (!request->priority)
    {
        return;
    }
    if (request->ice_controlled)
    {
        auto response = make_stun_binding_error_response(*request, 487, "Role Conflict", {}, ice_pwd_);
        if (!response.empty())
        {
            send_udp(std::move(response), endpoint);
        }
        return;
    }
    if (!request->ice_controlling)
    {
        return;
    }
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

    send_udp(std::move(response), endpoint);
}

void whip_session::handle_dtls(std::span<const std::uint8_t> packet)
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
            spdlog::error("webrtc whip media startup failed session {}", id_);
            shutdown();
        }
        return;
    }

    schedule_dtls_timeout();
}

void whip_session::handle_srtp(std::span<const std::uint8_t> packet)
{
    if (!srtp_)
    {
        return;
    }

    const bool rtcp = is_rtcp(packet);
    auto clear = rtcp ? srtp_->unprotect_rtcp(packet) : srtp_->unprotect_rtp(packet);
    if (!clear)
    {
        spdlog::debug("webrtc inbound srtp rejected session {} size {}", id_, packet.size());
        return;
    }

    const bool accepted = rtcp ? media_receiver_->input_rtcp(*clear) : media_receiver_->input_rtp(*clear);
    if (!accepted)
    {
        spdlog::error("webrtc whip media input failed session {} rtcp {}", id_, rtcp);
        shutdown();
    }
}

bool whip_session::startup_media()
{
    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->startup(*dtls_->srtp_keying_material()))
    {
        return false;
    }

    srtp_ = std::move(srtp);
    establishment_timer_.cancel();
    spdlog::info("webrtc srtp started session {}", id_);
    return true;
}

void whip_session::send_udp(std::vector<std::uint8_t> packet)
{
    if (!remote_endpoint_.has_value())
    {
        return;
    }
    send_udp(std::move(packet), *remote_endpoint_);
}

void whip_session::send_udp(std::vector<std::uint8_t> packet, boost::asio::ip::udp::endpoint endpoint)
{
    if (local_port_ == 0 || packet.empty())
    {
        return;
    }
    if (!udp_transport_->write(std::move(packet), std::move(endpoint)))
    {
        spdlog::warn("whip udp write queue full session {}", id_);
    }
}

void whip_session::schedule_dtls_timeout()
{
    if (local_port_ == 0 || dtls_->connected())
    {
        return;
    }

    const auto timeout = dtls_->timeout();
    if (!timeout)
    {
        return;
    }

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

void whip_session::handle_dtls_timeout()
{
    if (local_port_ == 0 || dtls_->connected())
    {
        return;
    }

    if (!dtls_->handle_timeout())
    {
        spdlog::error("webrtc dtls timeout failed session {}", id_);
        shutdown();
        return;
    }
    schedule_dtls_timeout();
}

void whip_session::startup_establishment_timeout()
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

void whip_session::refresh_ice_activity_timeout()
{
    ice_activity_timer_.expires_after(ice_activity_timeout);
    const auto self = shared_from_this();
    ice_activity_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || self->local_port_ == 0 || !self->remote_endpoint_.has_value())
            {
                return;
            }

            spdlog::info("webrtc ice activity timeout session {}", self->id_);
            self->shutdown();
        });
}

}    // namespace media_server
