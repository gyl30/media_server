#ifndef MEDIA_GB28181_GB28181_UDP_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/net/udp_socket.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_input_media.h"

namespace media_server
{

struct gb28181_udp_peer
{
    std::optional<boost::asio::ip::udp::endpoint> rtp;
    std::uint16_t rtcp_port{};
};

class gb28181_udp_session final : public stream_session, public std::enable_shared_from_this<gb28181_udp_session>
{
   public:
    gb28181_udp_session(boost::asio::any_io_executor executor, std::string stream_name, gb28181_description description, gb28181_udp_peer peer);

    [[nodiscard]] bool startup();
    void shutdown() override;

    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    void on_rtp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void on_rtcp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void wait_rtcp();
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    gb28181_description description_;
    gb28181_udp_peer peer_;
    gb28181_input_media media_;
    std::shared_ptr<udp_socket> rtp_socket_;
    std::shared_ptr<udp_socket> rtcp_socket_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
    bool closed_{};
};

}    // namespace media_server

#endif
