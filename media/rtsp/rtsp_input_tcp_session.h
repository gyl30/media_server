#ifndef MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>
#include <string_view>

#include <boost/system/error_code.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/rtsp/rtsp_input_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_input_session;

class rtsp_input_tcp_session final : public std::enable_shared_from_this<rtsp_input_tcp_session>
{
   public:
    rtsp_input_tcp_session(boost::asio::any_io_executor executor,
                           std::string stream_name,
                           std::string session_id,
                           std::vector<rtsp_input_track_description> descriptions,
                           std::function<void(std::span<const std::uint8_t>)> write);
    ~rtsp_input_tcp_session();

    void set_error_handle(std::function<void(boost::system::error_code)> handle) { error_handle_ = std::move(handle); }

   private:
    friend class rtsp_input_session;

    struct track_state
    {
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    int startup(rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_record(rtsp_server_t* server, std::string_view session);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    void wait_rtcp();
    void safe_shutdown();

    std::function<void(std::span<const std::uint8_t>)> write_;
    std::function<void(boost::system::error_code)> error_handle_;
    rtsp_input_media media_;
    std::string session_id_;
    std::vector<track_state> tracks_;
    boost::asio::steady_timer rtcp_timer_;
    bool recording_{};
    bool closed_{};
};

}    // namespace media_server

#endif
