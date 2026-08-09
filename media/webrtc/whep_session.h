#ifndef MEDIA_WEBRTC_WHEP_SESSION_H
#define MEDIA_WEBRTC_WHEP_SESSION_H

#include "media/core/media_stream.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/rtcp_receiver.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/webrtc_output.h"
#include "media/webrtc/webrtc_sdp.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace media_server
{

struct whep_rtcp_stats
{
    std::size_t receiver_reports{};
    std::size_t plis{};
};

struct whep_session_timeouts
{
    std::chrono::milliseconds establishment{15'000};
    std::chrono::milliseconds ice_activity{30'000};
};

class whep_session final : public std::enable_shared_from_this<whep_session>
{
   public:
    using closed_handler = std::function<void(const whep_session&)>;

    whep_session(
        boost::asio::io_context& io,
        std::shared_ptr<media_stream> stream,
        boost::asio::ip::address advertised_address,
        std::shared_ptr<dtls_certificate> certificate,
        closed_handler handler = {},
        whep_session_timeouts timeouts = {});

    bool start(webrtc_offer offer);
    void close();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& answer_sdp() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] bool ice_connected() const noexcept;
    [[nodiscard]] bool dtls_connected() const noexcept;
    [[nodiscard]] bool srtp_started() const noexcept;
    [[nodiscard]] const whep_rtcp_stats& rtcp_stats() const noexcept;

   private:
    void receive();
    void handle_packet(std::size_t size);
    void handle_stun(std::size_t size);
    void handle_dtls(std::size_t size);
    void handle_srtp(std::size_t size);
    bool start_media();
    void send_dtls(std::span<const std::uint8_t> packet);
    void send_rtp(std::span<const std::uint8_t> packet);
    void send_rtcp(std::span<const std::uint8_t> packet);
    void send_udp(std::vector<std::uint8_t> packet);
    void write_udp();
    void schedule_dtls_timeout();
    void handle_dtls_timeout();
    void start_establishment_timeout();
    void refresh_ice_activity_timeout();

    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    closed_handler closed_handler_;
    whep_session_timeouts timeouts_;
    std::shared_ptr<media_sink> stream_observer_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    rtcp_receiver rtcp_receiver_;
    whep_rtcp_stats rtcp_stats_;
    std::shared_ptr<webrtc_output> output_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::steady_timer dtls_timer_;
    boost::asio::steady_timer establishment_timer_;
    boost::asio::steady_timer ice_activity_timer_;
    std::array<std::uint8_t, 2048> receive_buffer_{};
    boost::asio::ip::udp::endpoint receive_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_endpoint_;
    std::string id_;
    std::string ice_ufrag_;
    std::string ice_pwd_;
    std::string remote_ice_ufrag_;
    std::string answer_sdp_;
    std::optional<int> video_payload_type_;
    std::optional<int> audio_payload_type_;
    std::optional<int> audio_channel_count_;
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> send_queue_;
    std::uint16_t local_port_{};
    bool started_{};
};

}    // namespace media_server

#endif
