#include "media/webrtc/whep_session.h"

#include "media/webrtc/stun_message.h"

#include <spdlog/spdlog.h>

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
        spdlog::debug("webrtc whep start rejected invalid state");
        return false;
    }

    const auto* media = transport_media(offer);
    if (media == nullptr || media->ice_ufrag.empty() || media->ice_pwd.empty())
    {
        spdlog::debug("webrtc whep start rejected missing ice transport attributes");
        return false;
    }

    boost::system::error_code error;
    const auto protocol = advertised_address_.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4();
    socket_.open(protocol, error);
    if (error)
    {
        spdlog::error("webrtc udp socket open failed {}", error.message());
        return false;
    }

    const auto bind_address = advertised_address_.is_v6()
        ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
        : boost::asio::ip::address(boost::asio::ip::address_v4::any());
    socket_.bind(boost::asio::ip::udp::endpoint(bind_address, 0), error);
    if (error)
    {
        spdlog::error("webrtc udp socket bind failed {}", error.message());
        close();
        return false;
    }

    const auto endpoint = socket_.local_endpoint(error);
    if (error || endpoint.port() == 0)
    {
        spdlog::error("webrtc udp local endpoint failed {}", error.message());
        close();
        return false;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
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
        spdlog::debug("webrtc answer create failed session {}", id_);
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
        spdlog::error("webrtc dtls transport start failed session {}", id_);
        close();
        return false;
    }

    spdlog::debug("webrtc session {} remote fingerprint {}", id_, media->fingerprint);

    offer_ = std::move(offer);
    answer_sdp_ = answer->sdp;
    video_payload_type_ = answer->video_payload_type;
    audio_payload_type_ = answer->audio_payload_type;
    started_ = true;

    spdlog::info(
        "webrtc whep session started {} stream {} candidate {} {}",
        id_,
        stream_->name(),
        advertised_address_.to_string(),
        local_port_);
    spdlog::debug(
        "webrtc session {} local_ufrag {} remote_ufrag {} video_pt {} audio_pt {}",
        id_,
        ice_ufrag_,
        remote_ice_ufrag_,
        video_payload_type_.value_or(-1),
        audio_payload_type_.value_or(-1));
    receive();
    return true;
}

void whep_session::close()
{
    const bool was_started = started_;
    started_ = false;
    ice_connected_ = false;
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    send_queue_.clear();
    send_in_progress_ = false;
    if (output_ && stream_)
    {
        stream_->remove_sink(output_.get());
    }
    output_.reset();
    if (srtp_)
    {
        srtp_->close();
        srtp_.reset();
    }
    dtls_timer_.cancel();
    if (dtls_)
    {
        dtls_->close();
        dtls_.reset();
    }
    answer_sdp_.clear();
    video_payload_type_.reset();
    audio_payload_type_.reset();
    local_port_ = 0;
    boost::system::error_code error;
    socket_.cancel(error);
    socket_.close(error);

    if (was_started)
    {
        spdlog::info("webrtc whep session closed {}", id_);
    }
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

bool whep_session::srtp_started() const noexcept
{
    return srtp_ != nullptr && srtp_->started();
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
            else if (error != boost::asio::error::operation_aborted)
            {
                spdlog::debug("webrtc udp receive failed session {} error {}", self->id_, error.message());
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

    if (!ice_connected_ || !remote_endpoint_.has_value() || receive_endpoint_ != *remote_endpoint_)
    {
        spdlog::trace(
            "webrtc udp packet dropped before ice nomination session {} remote {} {} size {}",
            id_,
            receive_endpoint_.address().to_string(),
            receive_endpoint_.port(),
            size);
        return;
    }

    if (dtls_transport::is_dtls_packet(packet))
    {
        spdlog::trace("webrtc dtls packet received session {} size {}", id_, size);
        handle_dtls(size);
        return;
    }

    if (srtp_transport::is_rtp_or_rtcp(packet))
    {
        spdlog::trace("webrtc srtp packet received session {} size {}", id_, size);
        handle_srtp(size);
        return;
    }

    spdlog::trace("webrtc unknown udp packet session {} size {} first_byte {}", id_, size, packet.front());
}

void whep_session::handle_stun(std::size_t size)
{
    const auto remote_address = receive_endpoint_.address().to_string();
    const auto remote_port = receive_endpoint_.port();
    spdlog::debug(
        "webrtc stun received session {} remote {} {} size {}",
        id_,
        remote_address,
        remote_port,
        size);

    const auto username = ice_ufrag_ + ":" + remote_ice_ufrag_;
    const auto request = parse_stun_binding_request(
        std::span<const std::uint8_t>(receive_buffer_.data(), size),
        username,
        ice_pwd_);
    if (!request)
    {
        spdlog::debug("webrtc stun rejected session {} remote {} {}", id_, remote_address, remote_port);
        return;
    }

    spdlog::debug(
        "webrtc stun valid session {} remote {} {} use_candidate {}",
        id_,
        remote_address,
        remote_port,
        request->use_candidate);

    const auto response = make_stun_binding_success_response(*request, receive_endpoint_, ice_pwd_);
    if (response.empty())
    {
        spdlog::error("webrtc stun response create failed session {}", id_);
        return;
    }

    if (request->use_candidate)
    {
        const bool changed = !remote_endpoint_.has_value() || *remote_endpoint_ != receive_endpoint_;
        remote_endpoint_ = receive_endpoint_;
        ice_connected_ = true;
        if (changed)
        {
            spdlog::info("webrtc ice connected session {} remote {} {}", id_, remote_address, remote_port);
        }
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>(response);
    const auto endpoint = receive_endpoint_;
    const auto session_id = id_;
    spdlog::trace("webrtc stun response send session {} remote {} {} size {}", session_id, remote_address, remote_port, data->size());
    socket_.async_send_to(
        boost::asio::buffer(*data),
        endpoint,
        [data, endpoint, session_id](boost::system::error_code error, std::size_t bytes) {
            static_cast<void>(data);
            if (error)
            {
                spdlog::debug(
                    "webrtc stun response send failed session {} remote {} {} error {}",
                    session_id,
                    endpoint.address().to_string(),
                    endpoint.port(),
                    error.message());
                return;
            }
            spdlog::trace("webrtc stun response sent session {} bytes {}", session_id, bytes);
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
        spdlog::error("webrtc dtls failed session {}", id_);
        close();
        return;
    }

    if (!was_connected && dtls_->connected())
    {
        dtls_timer_.cancel();
        spdlog::info("webrtc dtls connected session {}", id_);
        if (!start_media())
        {
            spdlog::error("webrtc srtp start failed session {}", id_);
            close();
        }
        return;
    }

    schedule_dtls_timeout();
}


void whep_session::handle_srtp(std::size_t size)
{
    if (!srtp_ || !srtp_->started())
    {
        return;
    }

    const auto packet = srtp_->unprotect(std::span<const std::uint8_t>(receive_buffer_.data(), size));
    if (!packet)
    {
        spdlog::debug("webrtc srtp unprotect failed session {} size {}", id_, size);
        return;
    }

    spdlog::trace("webrtc srtp unprotected session {} rtcp {} size {}", id_, packet->rtcp, packet->bytes.size());

    // 第一阶段只完成接收侧 SRTP/SRTCP 解密和校验。
    // RTCP 反馈不会触发关键帧请求，后续按需要增加统计处理。
}

bool whep_session::start_media()
{
    if (!dtls_ || !dtls_->connected() || !dtls_->srtp_keying_material() || !video_payload_type_)
    {
        return false;
    }

    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->start(*dtls_->srtp_keying_material()))
    {
        return false;
    }

    const auto weak = weak_from_this();
    auto output = std::make_shared<webrtc_output>(
        webrtc_output_config{.h264_payload_type = *video_payload_type_},
        [weak](std::span<const std::uint8_t> packet) {
            if (const auto self = weak.lock())
            {
                self->send_rtp(packet);
            }
        });

    srtp_ = std::move(srtp);
    output_ = std::move(output);
    if (!stream_->add_sink(output_))
    {
        output_.reset();
        srtp_->close();
        srtp_.reset();
        return false;
    }

    spdlog::info("webrtc srtp started session {}", id_);
    spdlog::debug("webrtc srtp profile session {} {}", id_, dtls_->srtp_keying_material()->profile);
    return true;
}

void whep_session::send_dtls(std::span<const std::uint8_t> packet)
{
    send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end()));
}

void whep_session::send_rtp(std::span<const std::uint8_t> packet)
{
    if (!srtp_ || !srtp_->started())
    {
        return;
    }

    spdlog::trace("webrtc rtp protect session {} plain_size {}", id_, packet.size());
    auto protected_packet = srtp_->protect_rtp(packet);
    if (!protected_packet)
    {
        spdlog::error("webrtc srtp protect failed session {}", id_);
        return;
    }
    spdlog::trace("webrtc rtp protected session {} protected_size {}", id_, protected_packet->size());
    send_udp(std::move(*protected_packet));
}

void whep_session::send_udp(std::vector<std::uint8_t> packet)
{
    if (!started_ || !socket_.is_open() || !remote_endpoint_.has_value() || packet.empty())
    {
        return;
    }

    send_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(packet)));
    spdlog::trace("webrtc udp queued session {} queue_size {}", id_, send_queue_.size());
    write_udp();
}

void whep_session::write_udp()
{
    if (send_in_progress_ || send_queue_.empty() || !started_ || !socket_.is_open() || !remote_endpoint_.has_value())
    {
        return;
    }

    send_in_progress_ = true;
    const auto data = send_queue_.front();
    const auto endpoint = *remote_endpoint_;
    const auto self = shared_from_this();
    socket_.async_send_to(
        boost::asio::buffer(*data),
        endpoint,
        [self, data](boost::system::error_code error, std::size_t) {
            static_cast<void>(data);
            if (!self->started_)
            {
                return;
            }

            self->send_in_progress_ = false;
            if (!self->send_queue_.empty())
            {
                self->send_queue_.pop_front();
            }
            if (error)
            {
                spdlog::debug("webrtc udp send failed session {} error {}", self->id_, error.message());
                self->close();
                return;
            }
            spdlog::trace("webrtc udp sent session {} bytes {}", self->id_, data->size());
            self->write_udp();
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

    spdlog::trace("webrtc dtls timeout scheduled session {} milliseconds {}", id_, timeout->count());
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

    spdlog::trace("webrtc dtls timeout fired session {}", id_);
    if (!dtls_->handle_timeout())
    {
        spdlog::error("webrtc dtls timeout failed session {}", id_);
        close();
        return;
    }
    schedule_dtls_timeout();
}

}    // namespace media_server
