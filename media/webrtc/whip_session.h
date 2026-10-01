#ifndef MEDIA_WEBRTC_WHIP_SESSION_H
#define MEDIA_WEBRTC_WHIP_SESSION_H

#include <span>
#include <deque>
#include <memory>
#include <string>
#include <expected>
#include <vector>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/srtp_transport.h"
#include "media/net/udp_yield_transport.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/whip_media_receiver.h"

namespace media_server
{
class worker_context;

enum class whip_session_startup_error
{
    invalid_offer,
    internal_error,
};

class whip_session final : public std::enable_shared_from_this<whip_session>
{
   public:
    whip_session(worker_context& worker, std::string stream_name);

   public:
    [[nodiscard]] std::expected<std::string, whip_session_startup_error> startup(webrtc_offer offer,
                                                      boost::asio::ip::address advertised_address,
                                                      std::shared_ptr<dtls_certificate> certificate);
    void shutdown();

   public:
    [[nodiscard]] const std::string& id() const noexcept;

   private:
    struct pending_datagram
    {
        std::vector<std::uint8_t> packet;
        boost::asio::ip::udp::endpoint endpoint;
    };

   private:
    void run_udp(boost::asio::yield_context yield);
    void run_udp_write(boost::asio::yield_context yield);
    void handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_dtls(std::span<const std::uint8_t> packet);
    void handle_srtp(std::span<const std::uint8_t> packet);
    bool startup_media();
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
    std::string stream_name_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    std::unique_ptr<whip_media_receiver> media_receiver_;
    udp_yield_transport udp_transport_;
    std::size_t queued_write_bytes_{};
    std::deque<pending_datagram> udp_write_queue_;
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
};

}    // namespace media_server

#endif
