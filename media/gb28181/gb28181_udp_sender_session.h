#ifndef MEDIA_GB28181_GB28181_UDP_SENDER_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_SENDER_SESSION_H

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/net/port_manager.h"
#include "media/net/udp_yield_transport.h"

namespace media_server
{

class worker_context;
class gb28181_rtp_sender;

class gb28181_udp_sender_session final : public stream_session, public std::enable_shared_from_this<gb28181_udp_sender_session>
{
   public:
    gb28181_udp_sender_session(worker_context& worker,
                               std::shared_ptr<media_stream> stream,
                               gb28181_description description,
                               boost::asio::ip::address bind_address,
                               std::string sender_id,
                               bool rtcp_enabled,
                               std::chrono::milliseconds rtcp_interval = std::chrono::milliseconds{25'000});

    [[nodiscard]] bool startup();
    void shutdown() override;

   private:
    [[nodiscard]] std::optional<port_manager_impl::port_pair> prepare_udp_transports(boost::asio::ip::address bind_address);
    void shutdown_udp_transports();
    void run_rtp_write(boost::asio::yield_context yield);
    void schedule_rtcp();
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string sender_id_;
    gb28181_description description_;
    boost::asio::ip::address bind_address_;
    boost::asio::ip::udp::endpoint remote_rtp_endpoint_;
    boost::asio::ip::udp::endpoint remote_rtcp_endpoint_;
    udp_yield_transport rtp_transport_;
    udp_yield_transport rtcp_transport_;
    boost::asio::steady_timer rtcp_timer_;
    std::chrono::milliseconds rtcp_interval_;
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::optional<port_manager_impl::port_pair> local_ports_;
    std::shared_ptr<gb28181_rtp_sender> sender_;
    void* rtcp_sender_{};
    bool rtcp_enabled_{};
    bool rtcp_started_{};
};

}    // namespace media_server

#endif
