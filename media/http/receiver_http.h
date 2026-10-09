#ifndef MEDIA_HTTP_RECEIVER_HTTP_H
#define MEDIA_HTTP_RECEIVER_HTTP_H

#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/url_view.hpp>

namespace media_server
{

using receiver_http_request = boost::beast::http::request<boost::beast::http::string_body>;
using receiver_http_response = boost::beast::http::response<boost::beast::http::string_body>;

// GET /receivers 列出 GB28181 接收与 RTSP 拉流会话；POST /receivers/delete 按身份关闭并在清理完成后返回。
[[nodiscard]] receiver_http_response handle_receiver_request(const receiver_http_request& request,
                                                             const boost::urls::url_view& target,
                                                             boost::asio::yield_context yield);

// 按身份关闭接收会话并等待其资源清理完成；身份不存在或属于其他代时返回 false。
[[nodiscard]] bool close_receiver(std::string_view stream_name, std::string_view stream_id, boost::asio::yield_context yield);

}    // namespace media_server

#endif
