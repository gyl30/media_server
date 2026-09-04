#ifndef MEDIA_GB28181_GB28181_UDP_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_SESSION_H

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
#include "media/gb28181/gb28181_input_media.h"

namespace media_server
{
class worker_context;

class gb28181_udp_session final : public stream_session, public std::enable_shared_from_this<gb28181_udp_session>
{
   public:
    gb28181_udp_session(worker_context& worker, std::string stream_name, gb28181_description description);

    [[nodiscard]] bool startup();
    void shutdown() override;

    [[nodiscard]] const std::string& stream_name() const noexcept;
    [[nodiscard]] std::optional<port_manager_impl::port_pair> local_ports() const noexcept;

   private:
    [[nodiscard]] std::optional<port_manager_impl::port_pair> prepare_udp_transports(boost::asio::ip::address bind_address);
    void run_rtp(boost::asio::yield_context yield);
    void run_rtcp(boost::asio::yield_context yield);
    void run_rtcp_sender(boost::asio::yield_context yield);
    void safe_shutdown();

    worker_context& worker_;
    gb28181_description description_;
    gb28181_input_media media_;
    udp_yield_transport rtp_transport_;
    udp_yield_transport rtcp_transport_;
    std::optional<port_manager_impl::port_pair> local_ports_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
    bool closed_{};
};

}    // namespace media_server

#endif
