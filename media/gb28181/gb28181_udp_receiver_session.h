#ifndef MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/media_port_pool.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/net/udp_yield_transport.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_udp_receiver_session final : public session, public std::enable_shared_from_this<gb28181_udp_receiver_session>
{
   public:
    gb28181_udp_receiver_session(worker_context& worker,
                                 std::string stream_name,
                                 gb28181_transport_config config,
                                 boost::asio::ip::address bind_address,
                                 std::chrono::milliseconds rtcp_interval = std::chrono::milliseconds{1'000});

   public:
    [[nodiscard]] bool startup();
    void shutdown();

   public:
    [[nodiscard]] std::optional<media_port_pool::port_pair> local_ports() const noexcept;

   private:
    [[nodiscard]] std::optional<media_port_pool::port_pair> prepare_udp_transports(boost::asio::ip::address bind_address);
    void run_rtp(boost::asio::yield_context yield);
    void run_rtcp(boost::asio::yield_context yield);
    void schedule_rtcp();

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    gb28181_transport_config config_;
    boost::asio::ip::address bind_address_;
    gb28181_rtp_receiver receiver_;
    udp_yield_transport rtp_transport_;
    udp_yield_transport rtcp_transport_;
    std::optional<media_port_pool::port_pair> local_ports_;
    boost::asio::steady_timer rtcp_timer_;
    std::chrono::milliseconds rtcp_interval_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
    bool started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
