#ifndef MEDIA_HTTP_HTTP_SESSION_H
#define MEDIA_HTTP_HTTP_SESSION_H

#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/http/http_flv_output.h"
#include "media/webrtc/whep_service.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace media_server
{
class http_session final : public std::enable_shared_from_this<http_session>
{
   public:
    http_session(boost::asio::ip::tcp::socket socket, stream_registry& registry, hls_service& hls, whep_service& whep);

    void start();
    void shutdown();

   private:
    struct flv_chunk;

    void read_request();
    void on_request(boost::system::error_code error, std::size_t bytes);
    void handle_request();
    void handle_flv(const boost::urls::url_view& target);
    void handle_hls(const boost::urls::url_view& target);
    void handle_whep(const std::vector<std::string>& segments);
    void handle_whep_post(const std::vector<std::string>& segments);
    void handle_whep_delete(const std::vector<std::string>& segments);
    void wait_hls_playlist(std::string stream_name);
    void check_hls_playlist();

    void send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body);
    void send_whep_error_response(boost::beast::http::status status, std::string body);
    void send_whep_options_response();
    void send_whep_response(std::string session_id, std::string answer_sdp);
    void send_whep_empty_response(boost::beast::http::status status);
    void send_binary_response(boost::beast::http::status status, std::string_view content_type, std::vector<std::uint8_t> body);
    void start_flv(std::shared_ptr<media_stream> stream);
    void read_flv_client();
    void enqueue_flv(std::span<const std::uint8_t> data);
    void write_flv_chunk();
    void finish_flv();
    void detach_flv();
    void safe_shutdown();

    [[nodiscard]] static std::vector<std::string> path_segments(const boost::urls::url_view& target);
    [[nodiscard]] static std::string join_segments(const std::vector<std::string>& segments, std::size_t begin, std::size_t end);

    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::request<boost::beast::http::string_body> request_;
    stream_registry& registry_;
    hls_service& hls_;
    whep_service& whep_;
    boost::asio::steady_timer hls_wait_timer_;
    std::chrono::steady_clock::time_point hls_wait_deadline_{};
    std::string hls_wait_stream_name_;
    std::shared_ptr<media_stream> media_stream_;
    std::shared_ptr<http_flv_output> flv_output_;
    std::deque<std::shared_ptr<flv_chunk>> flv_chunks_;
    std::array<std::uint8_t, 1> flv_read_buffer_{};
    bool flv_finishing_ = false;
    bool closed_ = false;
};
}    // namespace media_server

#endif
