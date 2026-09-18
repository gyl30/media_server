#ifndef MEDIA_RTMP_RTMP_SESSION_H
#define MEDIA_RTMP_RTMP_SESSION_H

#include <deque>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_yield_transport.h"
#include "media/codec/video_transcode_config.h"

struct rtmp_server_t;

namespace media_server
{

struct rtmp_target
{
    std::string stream_id;
    std::string stream_name;
};

[[nodiscard]] std::optional<rtmp_target> parse_rtmp_target(std::string_view app, std::string_view stream);

class worker_context;
class rtmp_publish_session;
class rtmp_play_session;

class rtmp_session final : public std::enable_shared_from_this<rtmp_session>
{
   public:
    rtmp_session(worker_context& worker,
                 boost::asio::ip::tcp::socket socket,
                 video_transcode_config video = {},
                 std::chrono::milliseconds initial_tracks_timeout = std::chrono::milliseconds{15'000},
                 std::size_t max_write_queue_bytes = 1024U * 1024U);
    void startup();
    void shutdown();

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int play_callback(void* param, const char* app, const char* stream, double start, double duration, std::uint8_t reset);
    static int pause_callback(void* param, int pause, std::uint32_t milliseconds);
    static int seek_callback(void* param, std::uint32_t milliseconds);
    static int publish_callback(void* param, const char* app, const char* stream, const char* type);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int duration_callback(void* param, const char* app, const char* stream, double* duration);

    void run(boost::asio::yield_context yield);
    void run_write(boost::asio::yield_context yield);
    void report_transport_error(const boost::system::error_code& error);
    void write(std::shared_ptr<std::vector<std::uint8_t>> data);
    int on_play(std::string app, std::string stream);
    int on_publish(std::string app, std::string stream);
    void run_play_claim(boost::asio::yield_context yield);
    void run_publish_claim(boost::asio::yield_context yield);
    void safe_shutdown();

   private:
    worker_context& worker_;
    tcp_yield_transport transport_;
    std::size_t max_write_queue_bytes_;
    std::size_t queued_write_bytes_{};
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::chrono::milliseconds initial_tracks_timeout_;
    video_transcode_config video_config_;
    rtmp_server_t* rtmp_context_{};
    std::shared_ptr<rtmp_publish_session> publish_;
    std::shared_ptr<rtmp_play_session> play_;
    std::string stream_id_;
    std::string stream_name_;
    bool claim_pending_{};
    bool closed_{};
};

}    // namespace media_server

#endif
