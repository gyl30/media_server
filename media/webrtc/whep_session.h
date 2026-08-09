#ifndef MEDIA_WEBRTC_WHEP_SESSION_H
#define MEDIA_WEBRTC_WHEP_SESSION_H

#include "media/core/media_stream.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/webrtc_sdp.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>

namespace media_server
{

class whep_session final : public std::enable_shared_from_this<whep_session>
{
   public:
    whep_session(
        boost::asio::io_context& io,
        std::shared_ptr<media_stream> stream,
        boost::asio::ip::address advertised_address,
        std::shared_ptr<dtls_certificate> certificate);

    bool start(webrtc_offer offer);
    void close();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& answer_sdp() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] bool ice_connected() const noexcept;
    [[nodiscard]] bool dtls_connected() const noexcept;
    [[nodiscard]] std::optional<boost::asio::ip::udp::endpoint> remote_endpoint() const;
    [[nodiscard]] const std::optional<dtls_srtp_keying_material>& srtp_keying_material() const noexcept;

   private:
    void receive();
    void handle_packet(std::size_t size);
    void handle_stun(std::size_t size);
    void handle_dtls(std::size_t size);
    void send_dtls(std::span<const std::uint8_t> packet);
    void schedule_dtls_timeout();
    void handle_dtls_timeout();

    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    std::unique_ptr<dtls_transport> dtls_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::steady_timer dtls_timer_;
    std::array<std::uint8_t, 2048> receive_buffer_{};
    boost::asio::ip::udp::endpoint receive_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_endpoint_;
    std::string id_;
    std::string ice_ufrag_;
    std::string ice_pwd_;
    std::string remote_ice_ufrag_;
    webrtc_offer offer_;
    std::string answer_sdp_;
    std::uint16_t local_port_{};
    bool started_{};
    bool ice_connected_{};
};

}    // namespace media_server

#endif
