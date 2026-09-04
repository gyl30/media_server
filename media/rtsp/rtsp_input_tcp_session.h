#ifndef MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_TCP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

#include <boost/system/error_code.hpp>

#include <boost/asio/steady_timer.hpp>

#include "media/rtsp/rtsp_input_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class rtsp_input_session;

class rtsp_input_tcp_session final : public std::enable_shared_from_this<rtsp_input_tcp_session>
{
   public:
    rtsp_input_tcp_session(worker_context& worker,
                           std::string stream_name,
                           std::vector<rtsp_input_track_description> descriptions,
                           std::function<void(std::span<const std::uint8_t>)> write);
    ~rtsp_input_tcp_session();

    void set_error_handler(std::function<void(boost::system::error_code)> handler) { error_handler_ = std::move(handler); }

   private:
    friend class rtsp_input_session;

    struct track_state
    {
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    int startup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_setup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    int on_record(rtsp_server_t* server);
    void schedule_rtcp();
    void safe_shutdown();

    worker_context& worker_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    std::function<void(boost::system::error_code)> error_handler_;
    rtsp_input_media media_;
    std::vector<track_state> track_states_;
    boost::asio::steady_timer rtcp_timer_;
    bool closed_{};
};

}    // namespace media_server

#endif
