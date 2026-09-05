#ifndef MEDIA_RTSP_RTSP_INPUT_UDP_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_UDP_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <optional>
#include <functional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/port_manager.h"
#include "media/net/udp_yield_transport.h"
#include "media/rtsp/rtsp_input_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class rtsp_publish_session;

class rtsp_input_udp_session final : public std::enable_shared_from_this<rtsp_input_udp_session>
{
   public:
    rtsp_input_udp_session(worker_context& worker,
                           boost::asio::ip::address bind_address,
                           std::string stream_name,
                           std::vector<rtsp_input_track_description> descriptions);

    void set_error_handler(std::function<void(boost::system::error_code)> handler) { error_handler_ = std::move(handler); }

   private:
    friend class rtsp_publish_session;

    struct track_state
    {
        std::optional<udp_yield_transport> rtp_transport;
        std::optional<udp_yield_transport> rtcp_transport;
        boost::asio::ip::udp::endpoint rtp_endpoint;
        boost::asio::ip::udp::endpoint rtcp_endpoint;
        std::optional<port_manager_impl::port_pair> local_ports;
    };

    int startup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    void run_rtp(std::size_t track_index, boost::asio::yield_context yield);
    void run_rtcp(std::size_t track_index, boost::asio::yield_context yield);
    void run_rtcp_sender(boost::asio::yield_context yield);
    int on_setup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    int on_record(rtsp_server_t* server);
    void safe_shutdown();

    worker_context& worker_;
    std::function<void(boost::system::error_code)> error_handler_;
    boost::asio::ip::address bind_address_;
    rtsp_input_media media_;
    std::vector<track_state> track_states_;
    boost::asio::steady_timer rtcp_timer_;
    bool closed_{};
};

}    // namespace media_server

#endif
