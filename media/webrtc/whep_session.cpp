#include "media/webrtc/whep_session.h"

#include "media/webrtc/stun_message.h"

#include <spdlog/spdlog.h>

#include <boost/asio/post.hpp>

#include <openssl/rand.h>

#include <algorithm>
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
      executor_(executor),
      dtls_timer_(executor),
      establishment_timer_(executor),
      ice_activity_timer_(executor)
{
}

whep_session_startup_error whep_session::startup(webrtc_offer offer)
{
    if (closed_ || started_ || !stream_ || !certificate_)
    {
        spdlog::error("webrtc whep startup rejected invalid state");
        return whep_session_startup_error::internal_error;
    }
    const auto source_tracks = stream_->tracks();
    if (source_tracks.empty())
    {
        spdlog::debug("webrtc whep startup rejected stream without tracks");
        return whep_session_startup_error::stream_not_ready;
    }

    const auto bind_address = advertised_address_.is_v6() ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
                                                          : boost::asio::ip::address(boost::asio::ip::address_v4::any());
    const auto weak = weak_from_this();
    udp_socket_ = std::make_shared<udp_socket>(executor_);
    if (!udp_socket_->startup(
            bind_address,
            [weak](boost::system::error_code error,
                   std::span<const std::uint8_t> packet,
                   const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (const auto self = weak.lock())
                {
                    self->on_udp_read(error, packet, endpoint);
                }
            },
            [weak](boost::system::error_code error, const boost::asio::ip::udp::endpoint& endpoint)
            {
                if (const auto self = weak.lock())
                {
                    self->on_udp_write_error(error, endpoint);
                }
            }))
    {
        spdlog::error("webrtc udp socket startup failed");
        shutdown();
        return whep_session_startup_error::internal_error;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
        shutdown();
        return whep_session_startup_error::internal_error;
    }

    local_port_ = udp_socket_->local_port();
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
        return whep_session_startup_error::invalid_offer;
    }
    const auto media = std::find_if(
        offer.media.begin(), offer.media.end(), [&answer](const webrtc_media_offer& value) { return value.mid == answer->transport_mid; });
    if (media == offer.media.end() || media->ice_ufrag.empty() || media->ice_pwd.empty() ||
        !dtls_transport::valid_sha256_fingerprint(media->fingerprint))
    {
        spdlog::debug("webrtc whep startup rejected invalid transport attributes");
        shutdown();
        return whep_session_startup_error::invalid_offer;
    }

    remote_ice_ufrag_ = media->ice_ufrag;
    dtls_ = std::make_unique<dtls_transport>(certificate_,
                                             media->fingerprint,
                                             [weak](std::span<const std::uint8_t> packet)
                                             {
                                                 if (const auto self = weak.lock())
                                                 {
                                                     self->send_dtls(packet);
                                                 }
                                             });
    if (!dtls_->startup())
    {
        spdlog::error("webrtc dtls transport startup failed session {}", id_);
        shutdown();
        return whep_session_startup_error::internal_error;
    }

    spdlog::debug("webrtc session {} remote fingerprint {}", id_, media->fingerprint);

    answer_sdp_ = answer->sdp;
    video_codec_ = answer->video_codec;
    video_payload_type_ = answer->video_payload_type;
    audio_payload_type_ = answer->audio_payload_type;
    audio_channel_count_ = answer->audio_channel_count;
    audio_bitrate_ = answer->audio_bitrate;
    audio_max_playback_rate_ = answer->audio_max_playback_rate;
    rtcp_stats_ = {};
    started_ = true;

    for (const auto& track : source_tracks)
    {
        const bool negotiated_video = video_codec_ && track.kind == media_kind::video && track.codec == *video_codec_;
        const bool negotiated_audio = audio_payload_type_ && track.kind == media_kind::audio && track.codec == codec_id::aac;
        if (negotiated_video || negotiated_audio)
        {
            track_versions_.emplace(track.id, track.config_version);
        }
    }
    reader_ = stream_->add_reader(shared_from_this(), executor_);

    spdlog::info("webrtc whep session started {} stream {} candidate {} {}", id_, stream_->name(), advertised_address_.to_string(), local_port_);
    spdlog::debug("webrtc session {} local_ufrag {} remote_ufrag {} video_pt {} audio_pt {} audio_channels {} audio_bitrate {} audio_max_playback_rate {}",
                  id_,
                  ice_ufrag_,
                  remote_ice_ufrag_,
                  video_payload_type_.value_or(-1),
                  audio_payload_type_.value_or(-1),
                  audio_channel_count_.value_or(0),
                  audio_bitrate_.value_or(0),
                  audio_max_playback_rate_.value_or(0));
    startup_establishment_timeout();
    return whep_session_startup_error::none;
}

void whep_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void whep_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    started_ = false;
    remote_endpoint_.reset();
    remote_ice_ufrag_.clear();
    reader_.remove();
    reader_ = {};
    output_.reset();
    pending_tracks_.clear();
    track_versions_.clear();
    reader_cursor_.reset();
    track_revision_ = 0;
    tracks_ready_ = false;
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
    audio_bitrate_.reset();
    audio_max_playback_rate_.reset();
    local_port_ = 0;
    if (udp_socket_)
    {
        udp_socket_->shutdown();
        udp_socket_.reset();
    }

    spdlog::info("webrtc whep session shutdown {}", id_);
}

const std::string& whep_session::id() const noexcept { return id_; }

const std::string& whep_session::answer_sdp() const noexcept { return answer_sdp_; }

std::uint16_t whep_session::local_port() const noexcept { return local_port_; }

bool whep_session::ice_connected() const noexcept { return remote_endpoint_.has_value(); }

bool whep_session::dtls_connected() const noexcept { return dtls_ != nullptr && dtls_->connected(); }

bool whep_session::srtp_started() const noexcept { return srtp_ != nullptr; }

const whep_rtcp_stats& whep_session::rtcp_stats() const noexcept { return rtcp_stats_; }

void whep_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_ || !started_)
    {
        return;
    }
    if (!apply_tracks(tracks))
    {
        spdlog::info("webrtc negotiated track changed session {}", id_);
        shutdown();
        return;
    }
    if (output_ && !start_media_read())
    {
        shutdown();
    }
}

void whep_session::on_read(media_read_batch batch)
{
    if (closed_ || !started_ || !output_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    if (!apply_tracks(batch.tracks))
    {
        spdlog::info("webrtc negotiated track changed session {}", id_);
        shutdown();
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto expected = track_versions_.find(entry.frame.track);
        if (expected == track_versions_.end() || expected->second != entry.config_version)
        {
            continue;
        }
        output_->on_frame(entry.frame);
    }

    if (!closed_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void whep_session::on_end()
{
    spdlog::info("webrtc source stream ended session {}", id_);
    shutdown();
}

bool whep_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return true;
    }

    std::vector<media_track> negotiated_tracks;
    negotiated_tracks.reserve(track_versions_.size());
    for (const auto& [id, version] : track_versions_)
    {
        const auto track = std::ranges::find_if(tracks->tracks, [id](const media_track& current) { return current.id == id; });
        if (track == tracks->tracks.end() || track->config_version != version)
        {
            return false;
        }
        negotiated_tracks.push_back(*track);
    }

    if (!tracks_ready_)
    {
        pending_tracks_ = std::move(negotiated_tracks);
        tracks_ready_ = true;
    }
    track_revision_ = tracks->revision;
    return true;
}

void whep_session::on_udp_read(boost::system::error_code error,
                               std::span<const std::uint8_t> packet,
                               const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!started_)
    {
        return;
    }
    if (error)
    {
        spdlog::debug("webrtc udp receive failed session {} error {}", id_, error.message());
        shutdown();
        return;
    }
    handle_packet(packet, endpoint);
}

void whep_session::on_udp_write_error(boost::system::error_code error, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!started_)
    {
        return;
    }
    spdlog::debug("webrtc udp send failed session {} remote {} {} error {}",
                  id_,
                  endpoint.address().to_string(),
                  endpoint.port(),
                  error.message());
    if (remote_endpoint_.has_value() && endpoint == *remote_endpoint_)
    {
        shutdown();
    }
}

void whep_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (packet.empty())
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
        spdlog::trace("webrtc srtp packet received session {} size {}", id_, packet.size());
        handle_srtp(packet);
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
    udp_socket_->send(std::move(response), endpoint);
}

void whep_session::handle_dtls(std::span<const std::uint8_t> packet)
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
            spdlog::error("webrtc srtp startup failed session {}", id_);
            shutdown();
        }
        return;
    }

    schedule_dtls_timeout();
}

void whep_session::handle_srtp(std::span<const std::uint8_t> packet)
{
    if (!srtp_)
    {
        return;
    }

    const auto clear_packet = srtp_->unprotect(packet);
    if (!clear_packet)
    {
        spdlog::debug("webrtc srtp unprotect failed session {} size {}", id_, packet.size());
        return;
    }

    spdlog::trace("webrtc srtp unprotected session {} rtcp {} size {}", id_, clear_packet->rtcp, clear_packet->bytes.size());

    if (!clear_packet->rtcp)
    {
        return;
    }

    rtcp_receive_result result;
    if (!rtcp_receiver_.input(clear_packet->bytes, result))
    {
        spdlog::debug("webrtc rtcp rejected session {} size {}", id_, clear_packet->bytes.size());
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

bool whep_session::startup_media()
{
    if (!dtls_ || !dtls_->connected() || !dtls_->srtp_keying_material() || (!video_payload_type_ && !audio_payload_type_))
    {
        return false;
    }

    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->startup(*dtls_->srtp_keying_material()))
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
            .opus_bitrate = audio_bitrate_.value_or(64'000 * audio_channel_count_.value_or(1)),
            .opus_max_playback_rate = audio_max_playback_rate_.value_or(48'000),
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
    if (tracks_ready_ && !start_media_read())
    {
        return false;
    }

    establishment_timer_.cancel();
    spdlog::info("webrtc srtp started session {}", id_);
    spdlog::debug("webrtc srtp profile session {} {}", id_, dtls_->srtp_keying_material()->profile);
    return true;
}

bool whep_session::start_media_read()
{
    if (!output_ || !tracks_ready_)
    {
        return false;
    }

    for (const auto& track : pending_tracks_)
    {
        output_->on_track(track);
    }
    if (!output_->valid())
    {
        return false;
    }
    pending_tracks_.clear();
    reader_handle().async_read(reader_cursor_);
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
    if (!started_ || !udp_socket_ || !remote_endpoint_.has_value() || packet.empty())
    {
        return;
    }

    udp_socket_->send(std::move(packet), *remote_endpoint_);
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

void whep_session::startup_establishment_timeout()
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
