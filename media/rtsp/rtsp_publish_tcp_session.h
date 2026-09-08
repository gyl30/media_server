#ifndef MEDIA_RTSP_RTSP_PUBLISH_TCP_SESSION_H
#define MEDIA_RTSP_RTSP_PUBLISH_TCP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

#include <boost/asio/steady_timer.hpp>

#include "media/rtsp/rtsp_publish_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class rtsp_publish_session;

class rtsp_publish_tcp_session final : public std::enable_shared_from_this<rtsp_publish_tcp_session>
{
   public:
    rtsp_publish_tcp_session(worker_context& worker,
                           std::string stream_name,
                           std::vector<rtsp_publish_track_description> descriptions,
                           std::function<void(std::span<const std::uint8_t>)> write);
    ~rtsp_publish_tcp_session();

   private:
    friend class rtsp_publish_session;

    struct track_state
    {
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    int startup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    [[nodiscard]] bool on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_setup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport, const std::string& session_id);
    int on_record(rtsp_server_t* server);
    void schedule_rtcp();
    void safe_shutdown();

    worker_context& worker_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    rtsp_publish_media media_;
    std::vector<track_state> track_states_;
    boost::asio::steady_timer rtcp_timer_;
    bool closed_{};
};

}    // namespace media_server

#endif
