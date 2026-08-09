#include "media/webrtc/whep_session.h"

#include "media/core/log.h"
#include "media/webrtc/stun_message.h"

#include <boost/asio/buffer.hpp>

#include <openssl/rand.h>

#include <array>
#include <utility>
#include <vector>

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
    std::string result;
    result.resize(bytes.size() * 2U);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

const webrtc_media_offer* transport_media(const webrtc_offer& offer)
{
    if (!offer.bundle_mids.empty())
    {
        for (const auto& media : offer.media)
        {
            if (media.mid == offer.bundle_mids.front())
            {
                return &media;
            }
        }
        return nullptr;
    }

    return offer.media.empty() ? nullptr : &offer.media.front();
}

}    // namespace

whep_session::whep_session(
    boost::asio::io_context& io,
    std::shared_ptr<media_stream> stream,
    boost::asio::ip::address advertised_address,
    std::shared_ptr<dtls_certificate> certificate)
    : stream_(std::move(stream)),
      advertised_address_(std::move(advertised_address)),
      certificate_(std::move(certificate)),
      socket_(io),
      dtls_timer_(io)
{
}

bool whep_session::start(webrtc_offer offer)
{
    if (started_ || !stream_ || !certificate_ || stream_->ended())
    {
        return false;
    }

    const auto* media = transport_media(offer);
    if (media == nullptr || media->ice_ufrag.empty() || media->ice_pwd.empty())
    {
        return false;
    }

    boost::system::error_code error;
    const auto protocol = advertised_address_.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4();
    socket_.open(protocol, error);
    if (error)
    {
        return false;
    }

    const auto bind_address = advertised_address_.is_v6()
        ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
        : boost::asio::ip::address(boost::asio::ip::address_v4::any());
    socket_.bind(boost::asio::ip::udp::endpoint(bind_address, 0), error);
    if (error)
    {
        close();
        return false;
    }

    const auto endpoint = socket_.local_endpoint(error);
    if (error || endpoint.port() == 0)
    {
        close();
        return false;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        close();
        return false;
    }

    local_port_ = endpoint.port();
    const auto answer = make_webrtc_answer(
        offer,
        stream_->tracks(),
        webrtc_answer_config{
            .address = advertised_address_,
            .port = local_port_,
            .ice_ufrag = ice_ufrag_,
            .ice_pwd = ice_pwd_,
            .fingerprint = certificate_->sha256_fingerprint(),
        });
    if (!answer)
    {
        close();
        return false;
    }

    remote_ice_ufrag_ = media->ice_ufrag;
    const auto weak = weak_from_this();
    dtls_ = std::make_unique<dtls_transport>(
        certificate_,
        media->fingerprint,
        [weak](std::span<const std::uint8_t> packet) {
            if (const auto self = weak.lock())
            {
                self->send_dtls(packet);
            }
        });
    if (!dtls_->start())
    {
        close();
        return false;
    }

    offer_ = std::move(offer);
    answer_sdp_ = *answer;
    started_ = true;
    receive();
    return true;
}

void whep_session::close()
{
    started_ = false;
    ice_connected_ = false;
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    boost::system::error_code timer_error;
    dtls_timer_.cancel(timer_error);
    if (dtls_)
    {
        dtls_->close();
        dtls_.reset();
    }
    answer_sdp_.clear();
    local_port_ = 0;
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);
}

const std::string& whep_session::id() const noexcept
{
    return id_;
}

const std::string& whep_session::answer_sdp() const noexcept
{
    return answer_sdp_;
}

std::uint16_t whep_session::local_port() const noexcept
{
    return local_port_;
}

bool whep_session::ice_connected() const noexcept
{
    return ice_connected_;
}

bool whep_session::dtls_connected() const noexcept
{
    return dtls_ != nullptr && dtls_->connected();
}

std::optional<boost::asio::ip::udp::endpoint> whep_session::remote_endpoint() const
{
    return remote_endpoint_;
}

const std::optional<dtls_srtp_keying_material>& whep_session::srtp_keying_material() const noexcept
{
    static const std::optional<dtls_srtp_keying_material> empty;
    return dtls_ == nullptr ? empty : dtls_->srtp_keying_material();
}

void whep_session::receive()
{
    if (!started_ || !socket_.is_open())
    {
        return;
    }

    const auto self = shared_from_this();
    socket_.async_receive_from(
        boost::asio::buffer(receive_buffer_),
        receive_endpoint_,
        [self](boost::system::error_code error, std::size_t size) {
            if (!error)
            {
                self->handle_packet(size);
            }
            if (self->started_ && error != boost::asio::error::operation_aborted)
            {
                self->receive();
            }
        });
}

void whep_session::handle_packet(std::size_t size)
{
    if (size == 0)
    {
        return;
    }

    const auto packet = std::span<const std::uint8_t>(receive_buffer_.data(), size);
    if (is_stun_message(packet))
    {
        handle_stun(size);
        return;
    }

    if (dtls_transport::is_dtls_packet(packet) &&
        ice_connected_ &&
        remote_endpoint_.has_value() &&
        receive_endpoint_ == *remote_endpoint_)
    {
        handle_dtls(size);
    }
}

void whep_session::handle_stun(std::size_t size)
{
    const auto username = ice_ufrag_ + ":" + remote_ice_ufrag_;
    const auto request = parse_stun_binding_request(
        std::span<const std::uint8_t>(receive_buffer_.data(), size),
        username,
        ice_pwd_);
    if (!request)
    {
        return;
    }

    const auto response = make_stun_binding_success_response(*request, receive_endpoint_, ice_pwd_);
    if (response.empty())
    {
        return;
    }

    if (request->use_candidate)
    {
        const bool changed = !remote_endpoint_.has_value() || *remote_endpoint_ != receive_endpoint_;
        remote_endpoint_ = receive_endpoint_;
        ice_connected_ = true;
        if (changed)
        {
            log_line("webrtc", "ice connected", receive_endpoint_.address().to_string(), receive_endpoint_.port());
        }
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>(response);
    const auto endpoint = receive_endpoint_;
    socket_.async_send_to(
        boost::asio::buffer(*data),
        endpoint,
        [data](boost::system::error_code, std::size_t) {
            static_cast<void>(data);
        });
}

void whep_session::handle_dtls(std::size_t size)
{
    if (!dtls_)
    {
        return;
    }

    const bool was_connected = dtls_->connected();
    if (!dtls_->handle_datagram(std::span<const std::uint8_t>(receive_buffer_.data(), size)))
    {
        log_line("webrtc", "dtls failed", id_);
        close();
        return;
    }

    if (!was_connected && dtls_->connected())
    {
        boost::system::error_code error;
        dtls_timer_.cancel(error);
        log_line("webrtc", "dtls connected", id_);
        return;
    }

    schedule_dtls_timeout();
}

void whep_session::send_dtls(std::span<const std::uint8_t> packet)
{
    if (!started_ || !socket_.is_open() || !remote_endpoint_.has_value() || packet.empty())
    {
        return;
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>(packet.begin(), packet.end());
    const auto endpoint = *remote_endpoint_;
    socket_.async_send_to(
        boost::asio::buffer(*data),
        endpoint,
        [data](boost::system::error_code, std::size_t) {
            static_cast<void>(data);
        });
}

void whep_session::schedule_dtls_timeout()
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
    dtls_timer_.async_wait([self](boost::system::error_code error) {
        if (!error)
        {
            self->handle_dtls_timeout();
        }
    });
}

void whep_session::handle_dtls_timeout()
{
    if (!started_ || !dtls_ || dtls_->connected())
    {
        return;
    }

    if (!dtls_->handle_timeout())
    {
        log_line("webrtc", "dtls timeout failed", id_);
        close();
        return;
    }
    schedule_dtls_timeout();
}

}    // namespace media_server
