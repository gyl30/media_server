#ifndef MEDIA_HTTP_HTTP_FLV_SESSION_H
#define MEDIA_HTTP_HTTP_FLV_SESSION_H

#include <memory>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "media/core/media_sink.h"
#include "media/flv/flv_muxer.h"

extern "C"
{
#include "flv-writer.h"
}

namespace media_server
{
class worker_context;
class http_flv_session final : public std::enable_shared_from_this<http_flv_session>
{
   public:
    using request_type = boost::beast::http::request<boost::beast::http::string_body>;

    http_flv_session(worker_context& worker, boost::beast::tcp_stream stream, request_type request);

   public:
    void startup();
    void shutdown();

   protected:
    void process_media_available();
    void handle_source_end();

   private:
    void run(boost::asio::yield_context yield);
    void handle_request(boost::asio::yield_context& yield);
    void send_text_response(boost::beast::http::status status,
                            std::string_view content_type,
                            std::string body,
                            boost::asio::yield_context& yield,
                            std::string_view allow = {});
    void enqueue(std::uint64_t generation, std::vector<std::uint8_t> data, bool bootstrap);
    void run_write(std::uint64_t generation, std::vector<std::uint8_t> data, boost::asio::yield_context yield);
    static int writer_callback(void* param, const flv_vec_t* vectors, int count);
    bool apply_tracks(const media_tracks_ptr& tracks);
    void process_read();
    void write_complete(std::uint64_t generation);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    boost::beast::tcp_stream stream_;
    request_type request_;
    bool closed_{};
    std::string stream_name_;
    std::vector<std::uint8_t> pending_bootstrap_;
    std::uint64_t pending_generation_{};
    bool pending_bootstrap_ready_{};
    bool write_in_progress_{};
    std::map<track_id, media_track> reader_tracks_;
    std::vector<std::uint8_t> output_buffer_;
    void* writer_ = nullptr;
    flv_muxer muxer_;
    std::shared_ptr<media_sink> sink_;
    std::uint64_t generation_{};
    bool waiting_for_key_frame_{};
};

}    // namespace media_server

#endif
