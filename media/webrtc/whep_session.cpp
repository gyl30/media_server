#include "media/webrtc/whep_session.h"

#include "media/webrtc/stun_message.h"

#include <spdlog/spdlog.h>

#include <boost/asio/post.hpp>
#include <boost/asio/buffer.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <map>
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

class whep_stream_observer final : public media_sink
{
   public:
    using close_handler = std::function<void()>;

    whep_stream_observer(std::span<const media_track> tracks, std::optional<codec_id> video_codec, bool audio, close_handler handler)
        : handler_(std::move(handler))
    {
        for (const auto& track : tracks)
        {
            const bool negotiated_video = video_codec && track.kind == media_kind::video && track.codec == *video_codec;
            const bool negotiated_audio = audio && track.kind == media_kind::audio && track.codec == codec_id::aac;
            if (negotiated_video || negotiated_audio)
            {
                config_versions_.emplace(track.id, track.config_version);
            }
        }
    }

    void on_track(const media_track& track) override
    {
        const auto iterator = config_versions_.find(track.id);
        if (iterator != config_versions_.end() && iterator->second != track.config_version)
        {
            close();
        }
    }

    void on_frame(const media_frame&) override {}

    void on_end() override { close(); }

   private:
    void close()
    {
        if (handler_)
        {
            handler_();
        }
    }

    std::map<track_id, std::uint64_t> config_versions_;
    close_handler handler_;
};

}    // namespace

whep_session::whep_session(boost::asio::any_io_executor executor,
                           std::shared_ptr<media_stream> stream,
                           boost::asio::ip::address advertised_address,
                           std::shared_ptr<dtls_certificate> certificate,
                           whep_session_timeouts timeouts)
    : stream_(std::move(stream)),
      advertised_address_(std::move(advertised_address)),
      certificate_(std::move(certificate)),
      timeouts_(timeouts),
      socket_(executor),
      dtls_timer_(executor),
      establishment_timer_(executor),
      ice_activity_timer_(executor)
{
}

whep_session_start_error whep_session::start(webrtc_offer offer)
{
    if (closed_ || started_ || !stream_ || !certificate_)
    {
        spdlog::error("webrtc whep start rejected invalid state");
        return whep_session_start_error::internal_error;
    }
    if (stream_->ended())
    {
        spdlog::debug("webrtc whep start rejected ended stream");
        return whep_session_start_error::stream_not_ready;
    }
    const auto source_tracks = stream_->tracks();
    if (source_tracks.empty())
    {
        spdlog::debug("webrtc whep start rejected stream without tracks");
        return whep_session_start_error::stream_not_ready;
    }

    boost::system::error_code error;
    const auto protocol = advertised_address_.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4();
    socket_.open(protocol, error);
    if (error)
    {
        spdlog::error("webrtc udp socket open failed {}", error.message());
        return whep_session_start_error::internal_error;
    }

    const auto bind_address = advertised_address_.is_v6() ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
                                                          : boost::asio::ip::address(boost::asio::ip::address_v4::any());
    socket_.bind(boost::asio::ip::udp::endpoint(bind_address, 0), error);
    if (error)
    {
        spdlog::error("webrtc udp socket bind failed {}", error.message());
        shutdown();
        return whep_session_start_error::internal_error;
    }

    const auto endpoint = socket_.local_endpoint(error);
    if (error || endpoint.port() == 0)
    {
        spdlog::error("webrtc udp local endpoint failed {}", error.message());
        shutdown();
        return whep_session_start_error::internal_error;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
        shutdown();
        return whep_session_start_error::internal_error;
    }

    local_port_ = endpoint.port();
    const auto answer = make_webrtc_answer(offer,
                                           source_tracks,
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
        shutdown();
        return whep_session_start_error::invalid_offer;
    }
    const auto media = std::find_if(
        offer.media.begin(), offer.media.end(), [&answer](const webrtc_media_offer& value) { return value.mid == answer->transport_mid; });
    if (media == offer.media.end() || media->ice_ufrag.empty() || media->ice_pwd.empty() ||
        !dtls_transport::valid_sha256_fingerprint(media->fingerprint))
    {
        spdlog::debug("webrtc whep start rejected invalid transport attributes");
        shutdown();
        return whep_session_start_error::invalid_offer;
    }

    remote_ice_ufrag_ = media->ice_ufrag;
    const auto weak = weak_from_this();
    dtls_ = std::make_unique<dtls_transport>(certificate_,
                                             media->fingerprint,
                                             [weak](std::span<const std::uint8_t> packet)
                                             {
                                                 if (const auto self = weak.lock())
                                                 {
                                                     self->send_dtls(packet);
                                                 }
                                             });
    if (!dtls_->start())
    {
        spdlog::error("webrtc dtls transport start failed session {}", id_);
        shutdown();
        return whep_session_start_error::internal_error;
    }

    spdlog::debug("webrtc session {} remote fingerprint {}", id_, media->fingerprint);

    answer_sdp_ = answer->sdp;
    video_codec_ = answer->video_codec;
    video_payload_type_ = answer->video_payload_type;
    audio_payload_type_ = answer->audio_payload_type;
    audio_channel_count_ = answer->audio_channel_count;
    rtcp_stats_ = {};
    started_ = true;

    const auto session_weak = weak_from_this();
    // WHEP SDP 固定，只观察本次 answer 实际协商的源轨道。
    stream_observer_ = std::make_shared<whep_stream_observer>(source_tracks,
                                                              video_codec_,
                                                              audio_payload_type_.has_value(),
                                                              [session_weak]()
                                                              {
                                                                  if (const auto self = session_weak.lock())
                                                                  {
                                                                      spdlog::info("webrtc source stream ended or negotiated track changed session {}", self->id_);
                                                                      self->shutdown();
                                                                  }
                                                              });
    stream_->add_sink(stream_observer_, socket_.get_executor());

    spdlog::info("webrtc whep session started {} stream {} candidate {} {}", id_, stream_->name(), advertised_address_.to_string(), local_port_);
    spdlog::debug("webrtc session {} local_ufrag {} remote_ufrag {} video_pt {} audio_pt {} audio_channels {}",
                  id_,
                  ice_ufrag_,
                  remote_ice_ufrag_,
                  video_payload_type_.value_or(-1),
                  audio_payload_type_.value_or(-1),
                  audio_channel_count_.value_or(0));
    start_establishment_timeout();
    receive();
    return whep_session_start_error::none;
}

void whep_session::shutdown()
{
    if (closed_.exchange(true))
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(),
                      [self]()
                      {
                          self->started_ = false;
                          self->safe_shutdown();
                      });
}

void whep_session::safe_shutdown()
{
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    send_queue_.clear();
    if (output_ && stream_)
    {
        stream_->remove_sink(*output_);
    }
    if (stream_observer_ && stream_)
    {
        stream_->remove_sink(*stream_observer_);
    }
    output_.reset();
    stream_observer_.reset();
    stream_.reset();
    certificate_.reset();
    srtp_.reset();
    dtls_timer_.cancel();
    establishment_timer_.cancel();
    ice_activity_timer_.cancel();
    dtls_.reset();
    answer_sdp_.clear();
    video_codec_.reset();
    video_payload_type_.reset();
    audio_payload_type_.reset();
    audio_channel_count_.reset();
    local_port_ = 0;
    boost::system::error_code error;
    socket_.close(error);

    spdlog::info("webrtc whep session shutdown {}", id_);
}

const std::string& whep_session::id() const noexcept { return id_; }

const std::string& whep_session::answer_sdp() const noexcept { return answer_sdp_; }

std::uint16_t whep_session::local_port() const noexcept { return local_port_; }

bool whep_session::ice_connected() const noexcept { return remote_endpoint_.has_value(); }

bool whep_session::dtls_connected() const noexcept { return dtls_ != nullptr && dtls_->connected(); }

bool whep_session::srtp_started() const noexcept { return srtp_ != nullptr; }

const whep_rtcp_stats& whep_session::rtcp_stats() const noexcept { return rtcp_stats_; }

void whep_session::receive()
{
    if (!started_ || !socket_.is_open())
    {
        return;
    }

    const auto self = shared_from_this();
    socket_.async_receive_from(boost::asio::buffer(receive_buffer_),
                               receive_endpoint_,
                               [self](boost::system::error_code error, std::size_t size)
                               {
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

    if (!remote_endpoint_.has_value() || receive_endpoint_ != *remote_endpoint_)
    {
        spdlog::trace("webrtc udp packet dropped before ice nomination session {} remote {} {} size {}",
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
    spdlog::debug("webrtc stun received session {} remote {} {} size {}", id_, remote_address, remote_port, size);

    const auto username = ice_ufrag_ + ":" + remote_ice_ufrag_;
    const auto request = parse_stun_binding_request(std::span<const std::uint8_t>(receive_buffer_.data(), size), username, ice_pwd_);
    if (!request)
    {
        spdlog::debug("webrtc stun rejected session {} remote {} {}", id_, remote_address, remote_port);
        return;
    }

    spdlog::debug("webrtc stun valid session {} remote {} {} use_candidate {}", id_, remote_address, remote_port, request->use_candidate);

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
        refresh_ice_activity_timeout();
        if (changed)
        {
            spdlog::info("webrtc ice connected session {} remote {} {}", id_, remote_address, remote_port);
        }
    }
    else if (remote_endpoint_.has_value() && receive_endpoint_ == *remote_endpoint_)
    {
        refresh_ice_activity_timeout();
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>(response);
    const auto endpoint = receive_endpoint_;
    const auto session_id = id_;
    spdlog::trace("webrtc stun response send session {} remote {} {} size {}", session_id, remote_address, remote_port, data->size());
    socket_.async_send_to(boost::asio::buffer(*data),
                          endpoint,
                          [data, endpoint, session_id](boost::system::error_code error, std::size_t bytes)
                          {
                              static_cast<void>(data);
                              if (error)
                              {
                                  spdlog::debug("webrtc stun response send failed session {} remote {} {} error {}",
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
        shutdown();
        return;
    }

    if (!was_connected && dtls_->connected())
    {
        dtls_timer_.cancel();
        spdlog::info("webrtc dtls connected session {}", id_);
        if (!start_media())
        {
            spdlog::error("webrtc srtp start failed session {}", id_);
            shutdown();
        }
        return;
    }

    schedule_dtls_timeout();
}

void whep_session::handle_srtp(std::size_t size)
{
    if (!srtp_)
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

    if (!packet->rtcp)
    {
        return;
    }

    rtcp_receive_result result;
    if (!rtcp_receiver_.input(packet->bytes, result))
    {
        spdlog::debug("webrtc rtcp rejected session {} size {}", id_, packet->bytes.size());
        return;
    }

    for (const auto& report : result.receiver_reports)
    {
        ++rtcp_stats_.receiver_reports;
        spdlog::trace(
            "webrtc rtcp receiver report session {} sender_ssrc {} source_ssrc {} fraction_lost {} cumulative_lost {} highest_sequence {} jitter {} "
            "lsr {} dlsr {}",
            id_,
            report.sender_ssrc,
            report.source_ssrc,
            report.fraction_lost,
            report.cumulative_lost,
            report.highest_sequence,
            report.jitter,
            report.lsr,
            report.dlsr);
    }

    for (const auto& pli : result.plis)
    {
        ++rtcp_stats_.plis;
        spdlog::trace("webrtc rtcp pli session {} sender_ssrc {} media_ssrc {}", id_, pli.sender_ssrc, pli.media_ssrc);
    }
}

bool whep_session::start_media()
{
    if (!dtls_ || !dtls_->connected() || !dtls_->srtp_keying_material() || (!video_payload_type_ && !audio_payload_type_))
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
        webrtc_output_config{
            .video_codec = video_codec_.value_or(codec_id::h264),
            .video_payload_type = video_payload_type_.value_or(-1),
            .opus_payload_type = audio_payload_type_.value_or(-1),
            .opus_channel_count = audio_channel_count_.value_or(1),
            .rtcp_cname = id_,
        },
        [weak](std::span<const std::uint8_t> packet)
        {
            if (const auto self = weak.lock())
            {
                self->send_rtp(packet);
            }
        },
        [weak](std::span<const std::uint8_t> packet)
        {
            if (const auto self = weak.lock())
            {
                self->send_rtcp(packet);
            }
        });

    if (!output->valid())
    {
        return false;
    }

    srtp_ = std::move(srtp);
    output_ = std::move(output);
    stream_->add_sink(output_, socket_.get_executor());

    establishment_timer_.cancel();
    spdlog::info("webrtc srtp started session {}", id_);
    spdlog::debug("webrtc srtp profile session {} {}", id_, dtls_->srtp_keying_material()->profile);
    return true;
}

void whep_session::send_dtls(std::span<const std::uint8_t> packet) { send_udp(std::vector<std::uint8_t>(packet.begin(), packet.end())); }

void whep_session::send_rtp(std::span<const std::uint8_t> packet)
{
    if (!srtp_)
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

void whep_session::send_rtcp(std::span<const std::uint8_t> packet)
{
    if (!srtp_)
    {
        return;
    }

    spdlog::trace("webrtc rtcp protect session {} plain_size {}", id_, packet.size());
    auto protected_packet = srtp_->protect_rtcp(packet);
    if (!protected_packet)
    {
        spdlog::error("webrtc srtcp protect failed session {}", id_);
        return;
    }
    spdlog::trace("webrtc rtcp protected session {} protected_size {}", id_, protected_packet->size());
    send_udp(std::move(*protected_packet));
}

void whep_session::send_udp(std::vector<std::uint8_t> packet)
{
    if (!started_ || !socket_.is_open() || !remote_endpoint_.has_value() || packet.empty())
    {
        return;
    }

    const bool start_write = send_queue_.empty();
    send_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(packet)));
    spdlog::trace("webrtc udp queued session {} queue_size {}", id_, send_queue_.size());
    if (start_write)
    {
        write_udp();
    }
}

void whep_session::write_udp()
{
    if (send_queue_.empty() || !started_ || !socket_.is_open() || !remote_endpoint_.has_value())
    {
        return;
    }

    const auto data = send_queue_.front();
    const auto endpoint = *remote_endpoint_;
    const auto self = shared_from_this();
    socket_.async_send_to(boost::asio::buffer(*data),
                          endpoint,
                          [self, data](boost::system::error_code error, std::size_t)
                          {
                              static_cast<void>(data);
                              if (!self->started_)
                              {
                                  return;
                              }

                              if (!self->send_queue_.empty())
                              {
                                  self->send_queue_.pop_front();
                              }
                              if (error)
                              {
                                  spdlog::debug("webrtc udp send failed session {} error {}", self->id_, error.message());
                                  self->shutdown();
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
    if (!started_ || !dtls_ || dtls_->connected())
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

void whep_session::start_establishment_timeout()
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

void whep_session::refresh_ice_activity_timeout()
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
