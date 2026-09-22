#ifndef MEDIA_SERVER_TESTS_CLIENTS_HTTP_FLV_TEST_CLIENT_H
#define MEDIA_SERVER_TESTS_CLIENTS_HTTP_FLV_TEST_CLIENT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

namespace media_server::test
{

class http_flv_test_client final
{
   public:
    explicit http_flv_test_client(boost::asio::io_context& io);

    boost::asio::awaitable<boost::system::error_code> play(std::string url);
    boost::asio::awaitable<boost::system::error_code> consume_one();
    void cancel() noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return received_bytes_; }
    [[nodiscard]] std::uint64_t received_messages() const noexcept { return received_messages_; }

   private:
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::response_parser<boost::beast::http::buffer_body> parser_;
    std::array<std::uint8_t, 3> signature_{};
    std::size_t signature_size_{};
    std::uint64_t received_bytes_{};
    std::uint64_t received_messages_{};
};

}    // namespace media_server::test

#endif
