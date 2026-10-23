#ifndef MEDIA_SERVER_BENCH_CLIENTS_HTTP_FLV_CLIENT_H
#define MEDIA_SERVER_BENCH_CLIENTS_HTTP_FLV_CLIENT_H

#include <span>
#include <array>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include "bench/packet_health.h"

namespace media_server::bench
{

class http_flv_client final
{
   public:
    http_flv_client(boost::asio::io_context& io, std::string path);

    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port, bool expect_audio);
    boost::asio::awaitable<boost::system::error_code> consume_one();
    void cancel() noexcept;
    [[nodiscard]] bool handshake_complete() const noexcept
    {
        return response_.is_header_done() && response_.get().result() == boost::beast::http::status::ok;
    }

    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return video_bytes_; }
    [[nodiscard]] std::uint64_t received_messages() const noexcept { return video_messages_; }
    [[nodiscard]] std::uint64_t received_audio_bytes() const noexcept { return audio_bytes_; }
    [[nodiscard]] std::uint64_t received_audio_messages() const noexcept { return audio_messages_; }
    [[nodiscard]] std::uint64_t video_media_ms() const noexcept { return video_timestamps_.advanced(); }

   private:
    boost::asio::awaitable<boost::system::error_code> read_exact(std::span<std::uint8_t> data);
    static int media_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags);

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    std::string path_;
    boost::beast::flat_buffer http_buffer_;
    boost::beast::http::response_parser<boost::beast::http::buffer_body> response_;
    std::array<std::uint8_t, 8192> body_buffer_{};
    std::size_t body_size_{};
    std::size_t body_offset_{};
    std::vector<std::uint8_t> tag_body_;
    timestamp_health video_timestamps_;
    timestamp_health audio_timestamps_;
    std::uint64_t video_bytes_{};
    std::uint64_t video_messages_{};
    std::uint64_t audio_bytes_{};
    std::uint64_t audio_messages_{};
};

}    // namespace media_server::bench

#endif
