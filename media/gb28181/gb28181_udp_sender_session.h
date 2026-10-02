#ifndef MEDIA_GB28181_GB28181_UDP_SENDER_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_SENDER_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/media_port_pool.h"
#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/net/udp_transport.h"
#include "media/net/worker_context.h"

namespace media_server
{

class gb28181_rtp_sender;

class gb28181_udp_sender_session final : public session, public std::enable_shared_from_this<gb28181_udp_sender_session>
{
   public:
    gb28181_udp_sender_session(worker_context& worker,
                               std::shared_ptr<media_stream> stream,
                               std::string sender_id,
                               boost::asio::ip::udp::endpoint remote_rtp_endpoint,
                               std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint);

   public:
    [[nodiscard]] bool startup(boost::asio::ip::address bind_address, std::uint8_t payload_type, std::uint32_t ssrc);
    void shutdown();

   private:
    void shutdown_udp_transports();
    void schedule_rtcp();
    void send_packet(std::vector<std::uint8_t> packet);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::string sender_id_;
    boost::asio::ip::udp::endpoint remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
    std::shared_ptr<udp_transport> rtp_transport_;
    std::shared_ptr<udp_transport> rtcp_transport_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<media_port_pool::port_pair> local_ports_;
    std::shared_ptr<gb28181_rtp_sender> sender_;
    void* rtcp_sender_{};
    bool rtcp_reporting_started_{};
    worker_context::shutdown_subscription shutdown_subscription_;
};

}    // namespace media_server

#endif
