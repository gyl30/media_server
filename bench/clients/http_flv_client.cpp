#include <limits>
#include <utility>
#include <algorithm>

#include <boost/asio/connect.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>

#include "bench/clients/http_flv_client.h"

extern "C"
{
#include "flv-proto.h"
#include "flv-parser.h"
}

namespace media_server::bench
{

http_flv_client::http_flv_client(boost::asio::io_context& io, std::string path)
    : resolver_(io), socket_(io), path_(path.starts_with('/') ? std::move(path) : "/" + path)
{
    response_.body_limit(std::numeric_limits<std::uint64_t>::max());
    http_buffer_.reserve(read_buffer_bytes);
}

boost::asio::awaitable<boost::system::error_code> http_flv_client::play(std::string host, std::uint16_t port, bool expect_audio)
{
    boost::system::error_code error;
    const auto endpoints =
        co_await resolver_.async_resolve(host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    co_await boost::asio::async_connect(socket_, endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    boost::beast::http::request<boost::beast::http::empty_body> request(boost::beast::http::verb::get, path_, 11);
    request.set(boost::beast::http::field::host, host + ':' + std::to_string(port));
    request.keep_alive(false);
    co_await boost::beast::http::async_write(socket_, request, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    co_await boost::beast::http::async_read_header(socket_, http_buffer_, response_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return error;
    }
    if (response_.get().result() != boost::beast::http::status::ok)
    {
        co_return response_.get().result() == boost::beast::http::status::forbidden
            ? boost::system::errc::make_error_code(boost::system::errc::permission_denied)
            : boost::asio::error::fault;
    }
    std::array<std::uint8_t, 9> file_header{};
    error = co_await read_exact(file_header);
    if (error)
    {
        co_return error;
    }
    flv_header_t header{};
    if (flv_header_read(&header, file_header.data(), file_header.size()) < 0 || header.offset != file_header.size())
    {
        co_return boost::asio::error::fault;
    }
    std::array<std::uint8_t, 4> previous_size{};
    error = co_await read_exact(previous_size);
    while (!error && (video_messages_ == 0 || (expect_audio && audio_messages_ == 0)))
    {
        error = co_await consume_one();
    }
    co_return error;
}

boost::asio::awaitable<boost::system::error_code> http_flv_client::consume_one()
{
    std::array<std::uint8_t, 11> tag_header{};
    auto error = co_await read_exact(tag_header);
    if (error)
    {
        co_return error;
    }
    flv_tag_header_t header{};
    if (flv_tag_header_read(&header, tag_header.data(), tag_header.size()) < 0)
    {
        co_return boost::asio::error::fault;
    }
    tag_body_.resize(header.size);
    error = co_await read_exact(tag_body_);
    if (error)
    {
        co_return error;
    }
    std::array<std::uint8_t, 4> previous_size{};
    error = co_await read_exact(previous_size);
    std::uint32_t tag_size{};
    if (error)
    {
        co_return error;
    }
    if (flv_tag_size_read(previous_size.data(), previous_size.size(), &tag_size) < 0 || tag_size != header.size + tag_header.size() ||
        flv_parser_tag(header.type, tag_body_.data(), tag_body_.size(), header.timestamp, &http_flv_client::media_callback, this) != 0)
    {
        co_return boost::asio::error::fault;
    }
    co_return boost::system::error_code{};
}

boost::asio::awaitable<boost::system::error_code> http_flv_client::read_exact(std::span<std::uint8_t> data)
{
    while (!data.empty())
    {
        if (body_offset_ == body_size_)
        {
            if (response_.is_done())
            {
                co_return boost::asio::error::eof;
            }
            response_.get().body().data = body_buffer_.data();
            response_.get().body().size = body_buffer_.size();
            boost::system::error_code error;
            co_await boost::beast::http::async_read_some(
                socket_, http_buffer_, response_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
            body_offset_ = 0;
            body_size_ = body_buffer_.size() - response_.get().body().size;
            if (error && error != boost::beast::http::error::need_buffer)
            {
                co_return error;
            }
            if (body_size_ == 0)
            {
                continue;
            }
        }
        const auto bytes = std::min(body_size_ - body_offset_, data.size());
        std::copy_n(body_buffer_.data() + body_offset_, bytes, data.data());
        body_offset_ += bytes;
        data = data.subspan(bytes);
    }
    co_return boost::system::error_code{};
}

void http_flv_client::cancel() noexcept
{
    resolver_.cancel();
    boost::system::error_code error;
    socket_.close(error);
}

int http_flv_client::media_callback(void* param, int codec, const void*, std::size_t bytes, std::uint32_t, std::uint32_t dts, int)
{
    auto* self = static_cast<http_flv_client*>(param);
    if (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265)
    {
        if (!self->video_timestamps_.observe(dts))
        {
            return -1;
        }
        self->video_bytes_ += bytes;
        ++self->video_messages_;
    }
    else if (codec == FLV_AUDIO_AAC || codec == FLV_AUDIO_OPUS || codec == FLV_AUDIO_G711A || codec == FLV_AUDIO_G711U)
    {
        if (!self->audio_timestamps_.observe(dts))
        {
            return -1;
        }
        self->audio_bytes_ += bytes;
        ++self->audio_messages_;
    }
    return 0;
}

}    // namespace media_server::bench
