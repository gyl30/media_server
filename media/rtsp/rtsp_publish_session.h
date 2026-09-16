#ifndef MEDIA_RTSP_RTSP_PUBLISH_SESSION_H
#define MEDIA_RTSP_RTSP_PUBLISH_SESSION_H

#include <span>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <functional>
#include <string_view>

#include <boost/asio/ip/address.hpp>

#include "media/core/runtime_event.h"
#include "media/rtsp/rtsp_publish_media.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class rtsp_publish_tcp_session;
class rtsp_publish_udp_session;

class rtsp_publish_session final
{
   public:
    using runtime_shutdown_handler = std::function<void(runtime_end_reason, std::string, std::string)>;
    using streaming_handler = std::function<void()>;

    rtsp_publish_session(worker_context& worker,
                         boost::asio::ip::address bind_address,
                         std::function<void(std::span<const std::uint8_t>)> write,
                         std::chrono::milliseconds rtcp_interval = std::chrono::milliseconds{1'000});

    void set_shutdown_handler(std::function<void()> handler) { shutdown_handler_ = std::move(handler); }
    void set_runtime_shutdown_handler(runtime_shutdown_handler handler) { runtime_shutdown_handler_ = std::move(handler); }
    void set_streaming_handler(streaming_handler handler) { streaming_handler_ = std::move(handler); }

    [[nodiscard]] bool on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    [[nodiscard]] const std::string& stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] const std::string& stream_name() const noexcept { return stream_name_; }
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session);
    int prepare_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length);
    int accept_announce(rtsp_server_t* server);
    int on_record(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale);
    void shutdown();

   private:
    void notify_shutdown(runtime_end_reason reason, std::string stage, std::string error);
    void notify_streaming_if_ready();

   private:
    worker_context& worker_;
    boost::asio::ip::address bind_address_;
    std::chrono::milliseconds rtcp_interval_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    std::function<void()> shutdown_handler_;
    runtime_shutdown_handler runtime_shutdown_handler_;
    streaming_handler streaming_handler_;
    std::shared_ptr<rtsp_publish_tcp_session> tcp_session_;
    std::shared_ptr<rtsp_publish_udp_session> udp_session_;
    std::vector<rtsp_publish_track_description> descriptions_;
    std::string stream_id_;
    std::string stream_name_;
    std::string session_id_;
    bool announce_prepared_{};
    bool streaming_notified_{};
    bool shutdown_notified_{};
};

}    // namespace media_server

#endif
