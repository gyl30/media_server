#ifndef MEDIA_RTSP_RTSP_INPUT_UDP_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_UDP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/net/udp_socket.h"
#include "media/rtsp/rtsp_input_media.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

class rtsp_input_udp_session final : public std::enable_shared_from_this<rtsp_input_udp_session>
{
   public:
    rtsp_input_udp_session(std::weak_ptr<rtsp_server_connection> connection,
                           boost::asio::any_io_executor executor,
                           stream_registry& registry,
                           std::string stream_name,
                           std::string session_id,
                           std::vector<rtsp_input_track_description> descriptions);

    int startup(rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    void shutdown();

   private:
    struct track_state
    {
        std::shared_ptr<udp_socket> rtp_socket;
        std::shared_ptr<udp_socket> rtcp_socket;
        boost::asio::ip::udp::endpoint rtp_endpoint;
        boost::asio::ip::udp::endpoint rtcp_endpoint;
    };

    [[nodiscard]] std::size_t on_read(std::span<const std::uint8_t> data);
    void on_rtp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    void on_rtcp(std::size_t track_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_record(rtsp_server_t* server, std::string_view session);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    void wait_rtcp();
    void safe_shutdown();

    std::weak_ptr<rtsp_server_connection> connection_;
    rtsp_input_media media_;
    std::string session_id_;
    std::vector<track_state> tracks_;
    boost::asio::steady_timer rtcp_timer_;
    bool recording_{};
    bool closed_{};
};

}    // namespace media_server

#endif
