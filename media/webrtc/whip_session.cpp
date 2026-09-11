#include <vector>
#include <cstddef>
#include <utility>
#include <algorithm>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/port_manager.h"
#include "media/webrtc/stun_message.h"
#include "media/net/worker_context.h"
#include "media/webrtc/whip_session.h"

namespace media_server
{
namespace
{

std::string random_hex(std::size_t byte_count)
{
    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    {
        return {};
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

bool is_rtcp(std::span<const std::uint8_t> packet)
{
    return packet.size() >= 2U && packet[1] >= 192U && packet[1] <= 223U;
}

}    // namespace

whip_session::whip_session(worker_context& worker,
                           std::string stream_name,
                           boost::asio::ip::address advertised_address,
                           std::shared_ptr<dtls_certificate> certificate,
                           whip_session_timeouts timeouts)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      advertised_address_(std::move(advertised_address)),
      certificate_(std::move(certificate)),
      timeouts_(timeouts),
      udp_transport_(worker_.io()),
      dtls_timer_(worker_.io()),
      establishment_timer_(worker_.io()),
      ice_activity_timer_(worker_.io())
{
}

whip_session_startup_error whip_session::startup(webrtc_offer offer)
{
    if (started_ || stream_name_.empty() || !certificate_)
    {
        spdlog::error("webrtc whip startup rejected invalid state");
        return whip_session_startup_error::internal_error;
    }
    if (advertised_address_.is_unspecified())
    {
        spdlog::error("webrtc whip startup rejected unspecified local address");
        return whip_session_startup_error::internal_error;
    }

    const auto reserved = port_manager::instance().acquire();
    if (!reserved)
    {
        spdlog::error("webrtc udp port allocation failed");
        return whip_session_startup_error::internal_error;
    }
    local_port_reservation_ = *reserved;

    boost::system::error_code udp_error;
    udp_transport_.startup(advertised_address_, local_port_reservation_, udp_error);
    if (udp_error)
    {
        port_manager::instance().release(local_port_reservation_);
        local_port_reservation_ = 0;
        spdlog::error("webrtc udp socket startup failed error {}", udp_error.message());
        shutdown();
        return whip_session_startup_error::internal_error;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
        shutdown();
        return whip_session_startup_error::internal_error;
    }

    local_port_ = local_port_reservation_;
    auto answer = make_whip_answer(offer,
                                   webrtc_answer_config{
                                       .address = advertised_address_,
                                       .port = local_port_,
                                       .stream_id = {},
                                       .ice_ufrag = ice_ufrag_,
                                       .ice_pwd = ice_pwd_,
                                       .fingerprint = certificate_->sha256_fingerprint(),
                                       .video = {},
                                   });
    if (!answer)
    {
        spdlog::debug("webrtc whip answer create failed session {}", id_);
        shutdown();
        return whip_session_startup_error::invalid_offer;
    }

    const auto media = std::find_if(
        offer.media.begin(), offer.media.end(), [&answer](const webrtc_media_offer& value) { return value.mid == answer->transport_mid; });
    if (media == offer.media.end() || media->ice_ufrag.empty() || media->ice_pwd.empty() ||
        !dtls_transport::valid_sha256_fingerprint(media->fingerprint))
    {
        spdlog::debug("webrtc whip startup rejected invalid transport attributes");
        shutdown();
        return whip_session_startup_error::invalid_offer;
    }

    remote_ice_ufrag_ = media->ice_ufrag;
    const auto self = shared_from_this();
    dtls_ = std::make_unique<dtls_transport>(certificate_,
                                             media->fingerprint,
                                             [self](std::span<const std::uint8_t> packet)
                                             { self->send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end())); });
    if (!dtls_->startup())
    {
        spdlog::error("webrtc dtls transport startup failed session {}", id_);
        shutdown();
        return whip_session_startup_error::internal_error;
    }

    answer_ = std::move(*answer);
    started_ = true;
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_udp(yield); }, boost::asio::detached);

    spdlog::info("webrtc whip session started {} stream {} candidate {} {}", id_, stream_name_, advertised_address_.to_string(), local_port_);
    startup_establishment_timeout();
    return whip_session_startup_error::none;
}

void whip_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void whip_session::safe_shutdown()
{
    if (!certificate_ && !media_receiver_)
    {
        return;
    }
    if (dtls_)
    {
        dtls_->shutdown();
    }
    started_ = false;
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    if (media_receiver_)
    {
        media_receiver_->shutdown();
        media_receiver_.reset();
    }
    if (srtp_)
    {
        srtp_->shutdown();
        srtp_.reset();
    }
    dtls_timer_.cancel();
    establishment_timer_.cancel();
    ice_activity_timer_.cancel();
    dtls_.reset();
    certificate_.reset();
    answer_ = {};
    local_port_ = 0;
    if (udp_write_queue_.empty())
    {
        shutdown_udp_transport();
    }

    spdlog::info("webrtc whip session shutdown {}", id_);
}

void whip_session::shutdown_udp_transport()
{
    udp_transport_.shutdown();
    local_port_ = 0;
    if (local_port_reservation_ != 0)
    {
        port_manager::instance().release(local_port_reservation_);
        local_port_reservation_ = 0;
    }
}

const std::string& whip_session::id() const noexcept { return id_; }

const std::string& whip_session::answer_sdp() const noexcept { return answer_.sdp; }

std::uint16_t whip_session::local_port() const noexcept { return local_port_; }

bool whip_session::ice_connected() const noexcept { return remote_endpoint_.has_value(); }

bool whip_session::dtls_connected() const noexcept { return dtls_ != nullptr && dtls_->connected(); }

bool whip_session::srtp_started() const noexcept { return srtp_ != nullptr && media_receiver_ != nullptr; }

void whip_session::run_udp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = udp_transport_.read(buffer, endpoint, yield, error);
        if (error)
        {
            if (error != boost::asio::error::operation_aborted)
            {
                spdlog::debug("webrtc udp receive failed session {} error {}", id_, error.message());
            }
            break;
        }
        handle_packet(std::span<const std::uint8_t>{buffer.data(), bytes}, endpoint);
    }
    shutdown();
}

void whip_session::run_udp_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (udp_write_queue_.empty())
        {
            if (local_port_ == 0)
            {
                shutdown_udp_transport();
            }
            return;
        }

        const auto datagram = udp_write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(
            udp_transport_.write(std::span<const std::uint8_t>{datagram.packet->data(), datagram.packet->size()}, datagram.endpoint, yield, error));
        if (error)
        {
            spdlog::debug("webrtc udp send failed session {} remote {} {} error {}",
                          id_,
                          datagram.endpoint.address().to_string(),
                          datagram.endpoint.port(),
                          error.message());
            udp_write_queue_.clear();
            shutdown_udp_transport();
            shutdown();
            return;
        }

        udp_write_queue_.pop_front();
    }
}

void whip_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!started_ || packet.empty())
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
    if (request->use_candidate && remote_endpoint_.has_value() && endpoint != *remote_endpoint_)
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
    if (!dtls_)
    {
        return;
    }

    const bool was_connected = dtls_->connected();
    if (!dtls_->handle_datagram(packet))
    {
        spdlog::error("webrtc dtls failed session {}", id_);
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
    if (!srtp_ || !media_receiver_)
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
    if (!dtls_ || !dtls_->connected() || !dtls_->srtp_keying_material() || !answer_.video_codec || !answer_.video_payload_type)
    {
        return false;
    }

    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->startup(*dtls_->srtp_keying_material()))
    {
        return false;
    }

    auto receiver = std::make_unique<whip_media_receiver>(
        worker_,
        stream_name_,
        whip_media_receiver_config{
            .video_codec = *answer_.video_codec,
            .video_payload_type = *answer_.video_payload_type,
            .audio_payload_type = answer_.audio_payload_type.value_or(-1),
            .audio_channel_count = static_cast<std::uint16_t>(answer_.audio_channel_count.value_or(2)),
        });
    if (!receiver->startup())
    {
        srtp->shutdown();
        return false;
    }

    srtp_ = std::move(srtp);
    media_receiver_ = std::move(receiver);
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
    if (!started_ || local_port_reservation_ == 0 || packet.empty())
    {
        return;
    }

    const bool start_write = udp_write_queue_.empty();
    udp_write_queue_.push_back(pending_datagram{
        .packet = std::make_shared<std::vector<std::uint8_t>>(std::move(packet)),
        .endpoint = std::move(endpoint),
    });
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_udp_write(yield); }, boost::asio::detached);
    }
}

void whip_session::schedule_dtls_timeout()
{
    if (!started_ || !dtls_ || dtls_->connected())
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
    if (!started_ || !dtls_ || dtls_->connected())
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
    if (!started_ || timeouts_.establishment.count() <= 0)
    {
        return;
    }

    establishment_timer_.expires_after(timeouts_.establishment);
    const auto self = shared_from_this();
    establishment_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || !self->started_ || self->srtp_started())
            {
                return;
            }

            spdlog::info("webrtc establishment timeout session {}", self->id_);
            self->shutdown();
        });
}

void whip_session::refresh_ice_activity_timeout()
{
    if (!started_ || !remote_endpoint_.has_value() || timeouts_.ice_activity.count() <= 0)
    {
        return;
    }

    ice_activity_timer_.expires_after(timeouts_.ice_activity);
    const auto self = shared_from_this();
    ice_activity_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || !self->started_ || !self->remote_endpoint_.has_value())
            {
                return;
            }

            spdlog::info("webrtc ice activity timeout session {}", self->id_);
            self->shutdown();
        });
}

}    // namespace media_server
