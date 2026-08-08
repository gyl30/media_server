#ifndef MEDIA_WEBRTC_WHEP_SESSION_H
#define MEDIA_WEBRTC_WHEP_SESSION_H

#include "media/core/media_stream.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/webrtc_sdp.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace media_server
{

class whep_session final
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

   private:
    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    boost::asio::ip::udp::socket socket_;
    std::string id_;
    std::string ice_ufrag_;
    std::string ice_pwd_;
    webrtc_offer offer_;
    std::string answer_sdp_;
    std::uint16_t local_port_{};
    bool started_{};
};

}    // namespace media_server

#endif
