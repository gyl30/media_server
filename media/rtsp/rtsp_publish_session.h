#ifndef MEDIA_RTSP_RTSP_PUBLISH_SESSION_H
#define MEDIA_RTSP_RTSP_PUBLISH_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <functional>
#include <string_view>

#include <boost/asio/steady_timer.hpp>

#include <boost/asio/ip/address.hpp>

#include "media/rtsp/rtsp_publish_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class rtsp_publish_udp_session;

class rtsp_publish_session final : public std::enable_shared_from_this<rtsp_publish_session>
{
   public:
    rtsp_publish_session(worker_context& worker,
                         boost::asio::ip::address bind_address,
                         std::function<void(std::span<const std::uint8_t>)> write,
                         std::function<void()> input_handler);

   public:
    void set_shutdown_handler(std::function<void()> handler) { shutdown_handler_ = std::move(handler); }

   public:
    void shutdown();

   public:
    [[nodiscard]] bool on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session);
    [[nodiscard]] bool on_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length);
    int on_record(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale);

   private:
    struct tcp_track_state
    {
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

   private:
    int on_tcp_setup(rtsp_server_t* server, std::size_t track_index, const rtsp_header_transport_t& transport);
    int on_tcp_record(rtsp_server_t* server);
    void schedule_tcp_rtcp();
    void safe_shutdown();

   private:
    worker_context& worker_;
    boost::asio::ip::address bind_address_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    std::function<void()> shutdown_handler_;
    std::function<void()> input_handler_;
    std::unique_ptr<rtsp_publish_media> tcp_media_;
    std::vector<tcp_track_state> tcp_track_states_;
    boost::asio::steady_timer tcp_rtcp_timer_;
    std::shared_ptr<rtsp_publish_udp_session> udp_session_;
    std::vector<rtsp_publish_track_description> descriptions_;
    std::string stream_id_;
    std::string session_id_;
};

}    // namespace media_server

#endif
