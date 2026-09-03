#ifndef MEDIA_GB28181_GB28181_UDP_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/udp_socket.h"
#include "media/net/port_manager.h"
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
    struct udp_socket_pair
    {
        std::shared_ptr<udp_socket> rtp;
        std::shared_ptr<udp_socket> rtcp;
        port_manager_impl::port_pair local_ports;
    };

    [[nodiscard]] std::optional<udp_socket_pair> prepare_udp_sockets(boost::asio::ip::address bind_address);
    void shutdown_udp_sockets();
    void on_rtp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void on_rtcp(std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void schedule_rtcp();
    void safe_shutdown();

    worker_context& worker_;
    gb28181_description description_;
    gb28181_input_media media_;
    std::shared_ptr<udp_socket> rtp_socket_;
    std::shared_ptr<udp_socket> rtcp_socket_;
    std::optional<port_manager_impl::port_pair> local_ports_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint_;
    std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint_;
    bool closed_{};
};

}    // namespace media_server

#endif
