#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

#include "tests/clients/http_flv_test_client.h"

namespace media_server::test
{
namespace
{

struct http_url
{
    std::string host;
    std::string port;
    std::string target;
};

boost::system::error_code parse_http_url(std::string_view value, http_url& result)
{
    constexpr std::string_view prefix = "http://";
    if (!value.starts_with(prefix))
    {
        return boost::asio::error::invalid_argument;
    }
    value.remove_prefix(prefix.size());
    const auto slash = value.find('/');
    const auto authority = value.substr(0, slash);
    if (authority.empty())
    {
        return boost::asio::error::invalid_argument;
    }
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos)
    {
        result.host = std::string(authority);
        result.port = "80";
    }
    else
    {
        result.host = std::string(authority.substr(0, colon));
        result.port = std::string(authority.substr(colon + 1U));
        if (result.host.empty() || result.port.empty())
        {
            return boost::asio::error::invalid_argument;
        }
    }
    result.target = slash == std::string_view::npos ? "/" : std::string(value.substr(slash));
    return {};
}

}    // namespace

http_flv_test_client::http_flv_test_client(boost::asio::io_context& io) : stream_(io) { parser_.body_limit(boost::none); }

boost::asio::awaitable<boost::system::error_code> http_flv_test_client::play(std::string url)
{
    namespace http = boost::beast::http;
    http_url endpoint;
    auto error = parse_http_url(url, endpoint);
    if (error)
    {
        co_return error;
    }

    boost::asio::ip::tcp::resolver resolver(stream_.get_executor());
    const auto endpoints =
        co_await resolver.async_resolve(endpoint.host, endpoint.port, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    stream_.expires_after(std::chrono::seconds{60});
    co_await stream_.async_connect(endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }

    http::request<http::empty_body> request{http::verb::get, endpoint.target, 11};
    request.set(http::field::host, endpoint.host);
    request.set(http::field::user_agent, "media-server-capacity-client");
    co_await http::async_write(stream_, request, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    co_await http::async_read_header(stream_, buffer_, parser_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    if (parser_.get().result() != http::status::ok || parser_.get()[http::field::content_type] != "video/x-flv")
    {
        co_return boost::asio::error::operation_aborted;
    }

    stream_.expires_never();
    while (received_bytes_ < 13U)
    {
        error = co_await consume_one();
        if (error)
        {
            co_return error;
        }
    }
    if (signature_ != std::array<std::uint8_t, 3>{'F', 'L', 'V'})
    {
        co_return boost::asio::error::operation_aborted;
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> http_flv_test_client::consume_one()
{
    namespace http = boost::beast::http;
    std::array<std::uint8_t, 16384> data{};
    parser_.get().body().data = data.data();
    parser_.get().body().size = data.size();
    boost::system::error_code error;
    co_await http::async_read_some(stream_, buffer_, parser_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    const auto bytes = data.size() - parser_.get().body().size;
    if (error == http::error::need_buffer)
    {
        error.clear();
    }
    if (error)
    {
        co_return error;
    }
    if (bytes == 0U)
    {
        if (parser_.is_done())
        {
            co_return boost::asio::error::eof;
        }
        co_return boost::system::error_code{};
    }
    const auto signature_bytes = std::min(bytes, signature_.size() - signature_size_);
    std::copy_n(data.begin(), signature_bytes, signature_.begin() + static_cast<std::ptrdiff_t>(signature_size_));
    signature_size_ += signature_bytes;
    received_bytes_ += bytes;
    ++received_messages_;
    co_return boost::system::error_code{};
}

void http_flv_test_client::cancel() noexcept
{
    boost::system::error_code error;
    stream_.socket().cancel(error);
}

void http_flv_test_client::close() noexcept
{
    boost::system::error_code error;
    stream_.socket().close(error);
}

}    // namespace media_server::test
