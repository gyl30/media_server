#ifndef MEDIA_WEBRTC_WHIP_SESSION_H
#define MEDIA_WEBRTC_WHIP_SESSION_H

#include <span>
#include <deque>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/udp_yield_transport.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/whip_media_receiver.h"

namespace media_server
{
class worker_context;

struct whip_session_timeouts
{
    std::chrono::milliseconds establishment{15'000};
    std::chrono::milliseconds ice_activity{30'000};
};

enum class whip_session_startup_error
{
    none,
    invalid_offer,
    internal_error,
};

class whip_session final : public std::enable_shared_from_this<whip_session>
{
   public:
    whip_session(worker_context& worker,
                 std::string stream_name,
                 boost::asio::ip::address advertised_address,
                 std::shared_ptr<dtls_certificate> certificate,
                 whip_session_timeouts timeouts = {});

    [[nodiscard]] whip_session_startup_error startup(webrtc_offer offer);
    void shutdown();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& answer_sdp() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] bool ice_connected() const noexcept;
    [[nodiscard]] bool dtls_connected() const noexcept;
    [[nodiscard]] bool srtp_started() const noexcept;

   private:
    struct pending_datagram
    {
        std::shared_ptr<std::vector<std::uint8_t>> packet;
        boost::asio::ip::udp::endpoint endpoint;
    };

    void safe_shutdown();
    void shutdown_udp_transport();
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

    worker_context& worker_;
    std::string stream_name_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    whip_session_timeouts timeouts_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    std::unique_ptr<whip_media_receiver> media_receiver_;
    udp_yield_transport udp_transport_;
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
    std::uint16_t local_port_{};
    bool started_{};
};

}    // namespace media_server

#endif
