#ifndef MEDIA_HTTP_HTTP_FLV_SESSION_H
#define MEDIA_HTTP_HTTP_FLV_SESSION_H

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "config.h"
#include "media/core/media_reader.h"

namespace media_server
{
class worker_context;
class http_flv_output;
class media_stream;

class http_flv_session final : public std::enable_shared_from_this<http_flv_session>
{
   public:
    using request_type = boost::beast::http::request<boost::beast::http::string_body>;

    http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& config);

    void startup();
    void shutdown();

   private:
    void handle_request();
    void write_string_response(std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response);
    void send_text_response(boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow = {});
    void startup_flv(std::shared_ptr<media_stream> stream);
    void read_client();
    void enqueue(std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap);
    void write_chunk(std::uint64_t generation, std::vector<std::uint8_t> data);
    void on_write(std::uint64_t generation, boost::system::error_code error);
    void safe_shutdown();

    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    request_type request_;
    const config& config_;
    std::shared_ptr<http_flv_output> output_;
    media_reader_handle reader_;
    std::vector<std::uint8_t> pending_bootstrap_;
    std::array<std::uint8_t, 1> read_buffer_{};
    std::uint64_t pending_generation_{};
    bool pending_bootstrap_ready_{};
    bool write_in_progress_{};
    bool closed_{};
};

}    // namespace media_server

#endif
