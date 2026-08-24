#ifndef MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/rtsp/rtsp_input_media.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "rtp-over-rtsp.h"
}

namespace media_server
{

class rtsp_input_tcp_session final : public std::enable_shared_from_this<rtsp_input_tcp_session>
{
   public:
    rtsp_input_tcp_session(std::weak_ptr<rtsp_server_connection> connection,
                           boost::asio::any_io_executor executor,
                           stream_registry& registry,
                           std::string stream_name,
                           std::string session_id,
                           std::vector<rtsp_input_track_description> descriptions);
    ~rtsp_input_tcp_session();

    int startup(rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    void shutdown();

   private:
    struct track_state
    {
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);

    [[nodiscard]] std::size_t on_read(std::span<const std::uint8_t> data);
    void on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes);
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
    rtp_over_rtsp_t interleaved_{};
    bool recording_{};
    bool closed_{};
};

}    // namespace media_server

#endif
