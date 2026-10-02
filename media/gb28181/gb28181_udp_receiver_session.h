#ifndef MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/media_port_pool.h"
#include "media/core/session_registry.h"
#include "media/net/udp_transport.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_udp_receiver_session final : public session, public std::enable_shared_from_this<gb28181_udp_receiver_session>
{
   public:
    gb28181_udp_receiver_session(worker_context& worker, std::string stream_name, std::uint8_t payload_type, std::uint32_t ssrc);

   public:
    [[nodiscard]] std::optional<std::uint16_t> startup(boost::asio::ip::address bind_address);
    void shutdown();

   private:
    void run_rtp(boost::asio::yield_context yield);
    void run_rtcp(boost::asio::yield_context yield);
    void schedule_rtcp();

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    gb28181_rtp_receiver receiver_;
    std::shared_ptr<udp_transport> rtp_transport_;
    std::shared_ptr<udp_transport> rtcp_transport_;
    std::optional<media_port_pool::port_pair> local_ports_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
};

}    // namespace media_server

#endif
