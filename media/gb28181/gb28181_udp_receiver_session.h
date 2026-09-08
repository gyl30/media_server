#ifndef MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_RECEIVER_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/port_manager.h"
#include "media/net/udp_yield_transport.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_udp_receiver_session final : public stream_session, public std::enable_shared_from_this<gb28181_udp_receiver_session>
{
   public:
    gb28181_udp_receiver_session(worker_context& worker,
                        std::string stream_name,
                        gb28181_transport_config config,
                        boost::asio::ip::address bind_address,
                        std::chrono::milliseconds rtcp_interval = std::chrono::milliseconds{1'000});

    [[nodiscard]] bool startup();
    void shutdown() override;

    [[nodiscard]] const std::string& stream_name() const noexcept;
    [[nodiscard]] std::optional<port_manager::port_pair> local_ports() const noexcept;

   private:
    [[nodiscard]] std::optional<port_manager::port_pair> prepare_udp_transports(boost::asio::ip::address bind_address);
    void run_rtp(boost::asio::yield_context yield);
    void run_rtcp(boost::asio::yield_context yield);
    void schedule_rtcp();
    void safe_shutdown();

    worker_context& worker_;
    gb28181_transport_config config_;
    boost::asio::ip::address bind_address_;
    gb28181_rtp_receiver receiver_;
    udp_yield_transport rtp_transport_;
    udp_yield_transport rtcp_transport_;
    std::optional<port_manager::port_pair> local_ports_;
    boost::asio::steady_timer rtcp_timer_;
    std::chrono::milliseconds rtcp_interval_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
};

}    // namespace media_server

#endif
