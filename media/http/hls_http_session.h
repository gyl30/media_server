#ifndef MEDIA_HTTP_HLS_HTTP_SESSION_H
#define MEDIA_HTTP_HLS_HTTP_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/steady_timer.hpp>

#include "config.h"

namespace media_server
{
class worker_context;
struct signaling_request_result;
class hls_play_session;
class hls_segmenter;

class hls_http_session final : public std::enable_shared_from_this<hls_http_session>
{
   public:
    using request_type = boost::beast::http::request<boost::beast::http::string_body>;

    hls_http_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config);

   public:
    void startup();
    void shutdown();

   private:
    void handle_request();
    void claim_play(std::string stream_id, std::string stream_name, std::string redirect_path);
    void handle_claim(std::string stream_id, std::string stream_name, std::string redirect_path, signaling_request_result result);
    void wait_for_playlist(std::shared_ptr<hls_play_session> viewer, std::shared_ptr<hls_segmenter> segmenter);
    void send_redirect(std::string location);
    void send_text_response(boost::beast::http::status status,
                            std::string_view content_type,
                            std::string body,
                            bool keep_alive,
                            std::string_view allow = {},
                            std::shared_ptr<hls_play_session> viewer = {},
                            bool mark_streaming = false);
    void send_binary_response(boost::beast::http::status status,
                              std::string_view content_type,
                              std::shared_ptr<const std::vector<std::uint8_t>> body,
                              bool keep_alive,
                              std::shared_ptr<hls_play_session> viewer);
    void response_completed(const boost::system::error_code& error,
                            bool keep_alive,
                            std::shared_ptr<hls_play_session> viewer = {},
                            bool mark_streaming = false);
    void read_request();

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    request_type request_;
    const config& config_;
    bool closed_{};
    boost::asio::steady_timer wait_timer_;
    std::chrono::steady_clock::time_point playlist_deadline_;
};

}    // namespace media_server

#endif
