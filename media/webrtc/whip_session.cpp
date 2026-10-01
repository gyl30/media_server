#include <vector>
#include <cstddef>
#include <chrono>
#include <utility>
#include <algorithm>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <boost/asio/dispatch.hpp>
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
constexpr std::size_t max_udp_write_queue_bytes = 1024U * 1024U;

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

bool is_rtcp(std::span<const std::uint8_t> packet) { return packet.size() >= 2U && packet[1] >= 192U && packet[1] <= 223U; }

}    // namespace

whip_session::whip_session(worker_context& worker, std::string stream_name)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      udp_transport_(worker_.io()),
      dtls_timer_(worker_.io()),
      establishment_timer_(worker_.io()),
      ice_activity_timer_(worker_.io())
{
}

whip_session_startup_error whip_session::startup(webrtc_offer offer,
                                                 boost::asio::ip::address advertised_address,
                                                 std::shared_ptr<dtls_certificate> certificate)
{
    boost::system::error_code udp_error;
    const auto reserved = media_port_pool::instance().acquire_and_bind(udp_transport_, advertised_address, udp_error);
    if (!reserved)
    {
        spdlog::error("webrtc udp socket startup failed error {}", udp_error ? udp_error.message() : "no available media port");
        return whip_session_startup_error::internal_error;
    }
    local_port_reservation_ = *reserved;

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
        shutdown();
        return whip_session_startup_error::internal_error;
    }

    auto answer = make_whip_answer(offer,
                                   webrtc_answer_config{
                                       .address = advertised_address,
                                       .port = local_port_reservation_,
                                       .stream_id = {},
                                       .ice_ufrag = ice_ufrag_,
                                       .ice_pwd = ice_pwd_,
                                       .fingerprint = certificate->sha256_fingerprint(),
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
    dtls_ = std::make_unique<dtls_transport>(media->fingerprint,
                                             [self](std::span<const std::uint8_t> packet)
                                             { self->send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end())); });
    if (!dtls_->startup(*certificate))
    {
        spdlog::error("webrtc dtls transport startup failed session {}", id_);
        shutdown();
        return whip_session_startup_error::internal_error;
    }

    answer_ = std::move(*answer);
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_udp(yield); });

    spdlog::info("webrtc whip session started {} stream {} candidate {} {}", id_, stream_name_, advertised_address.to_string(), local_port_reservation_);
    startup_establishment_timeout();
    return whip_session_startup_error::none;
}

void whip_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::dispatch(worker_.io(), [self]() { self->safe_shutdown(); });
}

void whip_session::safe_shutdown()
{
    if (local_port_reservation_ == 0)
    {
        return;
    }
    dtls_.reset();
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    media_receiver_.reset();
    srtp_.reset();
    dtls_timer_.cancel();
    establishment_timer_.cancel();
    ice_activity_timer_.cancel();
    answer_ = {};
    udp_transport_.shutdown();
    if (local_port_reservation_ != 0)
    {
        media_port_pool::instance().release(local_port_reservation_);
        local_port_reservation_ = 0;
    }

    spdlog::info("webrtc whip session shutdown {}", id_);
}

const std::string& whip_session::id() const noexcept { return id_; }

const std::string& whip_session::answer_sdp() const noexcept { return answer_.sdp; }

void whip_session::run_udp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    boost::system::error_code read_error;
    for (;;)
    {
        const auto bytes = udp_transport_.read(buffer, endpoint, yield, read_error);
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

void whip_session::run_udp_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (local_port_reservation_ == 0 || udp_write_queue_.empty())
        {
            return;
        }

        const auto& datagram = udp_write_queue_.front();
        boost::system::error_code error;
        udp_transport_.write(std::span<const std::uint8_t>{datagram.packet.data(), datagram.packet.size()}, datagram.endpoint, yield, error);
        if (error)
        {
            if (error == boost::asio::error::operation_aborted)
            {
                shutdown();
                return;
            }
            spdlog::debug("webrtc udp send failed session {} remote {} {} error {}",
                          id_,
                          datagram.endpoint.address().to_string(),
                          datagram.endpoint.port(),
                          error.message());
            shutdown();
            return;
        }
        if (local_port_reservation_ == 0)
        {
            return;
        }

        queued_write_bytes_ -= datagram.packet.size();
        udp_write_queue_.pop_front();
    }
}

void whip_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (local_port_reservation_ == 0 || packet.empty())
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
    if (!media_receiver_)
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

    auto receiver = std::make_unique<whip_media_receiver>(worker_, stream_name_);
    if (!receiver->startup(whip_media_receiver_config{
            .video_codec = *answer_.video_codec,
            .video_payload_type = *answer_.video_payload_type,
            .audio_payload_type = answer_.audio_payload_type.value_or(-1),
            .audio_channel_count = static_cast<std::uint16_t>(answer_.audio_channel_count.value_or(2)),
        }))
    {
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
    if (local_port_reservation_ == 0 || packet.empty())
    {
        return;
    }
    if (packet.size() > max_udp_write_queue_bytes || queued_write_bytes_ > max_udp_write_queue_bytes - packet.size())
    {
        spdlog::warn(
            "whip udp write queue full session {} queued {} limit {} dropped {}", id_, queued_write_bytes_, max_udp_write_queue_bytes, packet.size());
        return;
    }

    const bool start_write = udp_write_queue_.empty();
    queued_write_bytes_ += packet.size();
    udp_write_queue_.push_back(pending_datagram{
        .packet = std::move(packet),
        .endpoint = std::move(endpoint),
    });
    if (start_write)
    {
        const auto self = shared_from_this();
        worker_.spawn([self](boost::asio::yield_context yield) { self->run_udp_write(yield); });
    }
}

void whip_session::schedule_dtls_timeout()
{
    if (local_port_reservation_ == 0 || dtls_->connected())
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
    if (local_port_reservation_ == 0 || dtls_->connected())
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
            if (error || self->local_port_reservation_ == 0 || self->media_receiver_)
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
            if (error || self->local_port_reservation_ == 0 || !self->remote_endpoint_.has_value())
            {
                return;
            }

            spdlog::info("webrtc ice activity timeout session {}", self->id_);
            self->shutdown();
        });
}

}    // namespace media_server
