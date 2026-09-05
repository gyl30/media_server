#ifndef MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H

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
class gb28181_output_media;

class gb28181_udp_output_session final : public stream_session, public std::enable_shared_from_this<gb28181_udp_output_session>
{
   public:
    gb28181_udp_output_session(worker_context& worker,
                               std::shared_ptr<media_stream> stream,
                               gb28181_description description,
                               boost::asio::ip::address bind_address,
                               std::string output_id,
                               bool rtcp_enabled);

    [[nodiscard]] bool startup();
    void shutdown() override;

   private:
    [[nodiscard]] std::optional<port_manager_impl::port_pair> prepare_udp_transports(boost::asio::ip::address bind_address);
    void shutdown_udp_transports();
    void run_rtp_write(boost::asio::yield_context yield);
    void run_rtcp_sender(boost::asio::yield_context yield);
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string output_id_;
    gb28181_description description_;
    boost::asio::ip::address bind_address_;
    boost::asio::ip::udp::endpoint remote_rtp_endpoint_;
    boost::asio::ip::udp::endpoint remote_rtcp_endpoint_;
    udp_yield_transport rtp_transport_;
    udp_yield_transport rtcp_transport_;
    boost::asio::steady_timer rtcp_timer_;
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::optional<port_manager_impl::port_pair> local_ports_;
    std::shared_ptr<gb28181_output_media> media_;
    void* rtcp_sender_{};
    bool rtcp_enabled_{};
    bool rtcp_started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
