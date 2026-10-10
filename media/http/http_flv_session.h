#ifndef MEDIA_HTTP_HTTP_FLV_SESSION_H
#define MEDIA_HTTP_HTTP_FLV_SESSION_H

#include <memory>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "media/core/media_sink.h"
#include "media/flv/flv_muxer.h"
#include "config.h"

extern "C"
{
#include "flv-writer.h"
}

namespace media_server
{
class worker_context;
class media_stream;
class http_flv_session final : public media_sink, public std::enable_shared_from_this<http_flv_session>
{
   public:
    using request_type = boost::beast::http::request<boost::beast::http::string_body>;

    http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request, const config& application_config);

   public:
    void startup();

   private:
    void run(boost::asio::yield_context yield);
    void handle_request(boost::asio::yield_context& yield);
    void send_text_response(boost::beast::http::status status,
                            std::string_view content_type,
                            std::string body,
                            boost::asio::yield_context& yield,
                            std::string_view allow = {});
    [[nodiscard]] bool enqueue(std::vector<std::uint8_t> data);
    void run_write(boost::asio::yield_context yield);
    static int writer_callback(void* param, const flv_vec_t* vectors, int count);

   private:
    void shutdown();
    void safe_shutdown();

   private:
    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    request_type request_;
    const config& config_;
    std::deque<std::vector<std::uint8_t>> output_queue_;
    std::size_t queued_output_bytes_{};
    std::optional<track_id> waiting_video_track_;
    std::vector<std::uint8_t> output_buffer_;
    void* writer_ = nullptr;
    std::optional<flv_muxer> muxer_;
    std::shared_ptr<media_stream> source_;
    static constexpr std::size_t max_queued_output_bytes_ = 4U * 1024U * 1024U;

   public:
    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override;
    void on_end() override;
};

}    // namespace media_server

#endif
