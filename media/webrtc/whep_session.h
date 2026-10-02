#ifndef MEDIA_WEBRTC_WHEP_SESSION_H
#define MEDIA_WEBRTC_WHEP_SESSION_H

#include <atomic>
#include <span>
#include <memory>
#include <string>
#include <expected>
#include <vector>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/media_port_pool.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/srtp_transport.h"
#include "media/net/udp_transport.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/webrtc_packetizer.h"

namespace media_server
{
class worker_context;
class whep_audio_egress;

enum class whep_session_startup_error
{
    invalid_offer,
    internal_error,
};

class whep_session final : public media_sink, public std::enable_shared_from_this<whep_session>
{
   public:
    whep_session(worker_context& worker, std::shared_ptr<media_stream> stream);

   public:
    [[nodiscard]] std::expected<std::string, whep_session_startup_error> startup(webrtc_offer offer,
                                                      boost::asio::ip::address advertised_address,
                                                      std::shared_ptr<dtls_certificate> certificate);
    void shutdown();

   public:
    [[nodiscard]] const std::string& id() const noexcept;

   public:
    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   private:
    void run_udp(boost::asio::yield_context yield);
    void handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_dtls(std::span<const std::uint8_t> packet);
    bool startup_media();
    int send_rtp(std::span<const std::uint8_t> packet);
    int send_rtcp(std::span<const std::uint8_t> packet);
    void send_udp(std::vector<std::uint8_t> packet);
    void send_udp(std::vector<std::uint8_t> packet, boost::asio::ip::udp::endpoint endpoint);
    void schedule_dtls_timeout();
    void handle_dtls_timeout();
    void startup_establishment_timeout();
    void refresh_ice_activity_timeout();

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::shared_ptr<whep_audio_egress> audio_egress_;
    std::optional<track_id> waiting_video_track_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    std::unique_ptr<webrtc_packetizer> packetizer_;
    std::shared_ptr<udp_transport> udp_transport_;
    boost::asio::steady_timer dtls_timer_;
    boost::asio::steady_timer establishment_timer_;
    boost::asio::steady_timer ice_activity_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_endpoint_;
    std::uint16_t local_port_reservation_{};
    std::string id_;
    std::string ice_ufrag_;
    std::string ice_pwd_;
    std::string remote_ice_ufrag_;
    webrtc_answer answer_;
    std::atomic_bool shutdown_requested_{};
};

}    // namespace media_server

#endif
