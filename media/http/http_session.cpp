#include <chrono>
#include <utility>

#include <openssl/crypto.h>

#include <boost/json.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include "media/http/whep_http.h"
#include "media/http/whip_http.h"
#include "media/http/gb28181_http.h"
#include "media/http/http_session.h"
#include "media/net/worker_context.h"
#include "media/core/session_registry.h"
#include "media/http/rtsp_pull_http.h"
#include "media/http/hls_http_session.h"
#include "media/http/http_flv_session.h"
#include "media/http/http_target.h"
#include "media/webrtc/whep.h"
#include "media/webrtc/whip.h"

namespace media_server
{
namespace
{

// 控制面对账使用：列出仍在运行的输入会话。
boost::beast::http::response<boost::beast::http::string_body> make_receiver_list_response(
    const boost::beast::http::request<boost::beast::http::string_body>& request)
{
    boost::json::array receivers;
    for (const auto& receiver : session_registry::instance().receivers())
    {
        receivers.push_back(boost::json::object{{"stream_id", receiver}});
    }
    boost::beast::http::response<boost::beast::http::string_body> response(boost::beast::http::status::ok, request.version());
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, "application/json");
    response.keep_alive(false);
    response.body() = boost::json::serialize(boost::json::object{{"receivers", std::move(receivers)}});
    response.prepare_payload();
    return response;
}

bool is_control_path(std::string_view path)
{
    return path == "/receivers" || path.starts_with("/receivers/") || path == "/gb28181/receiver" || path.starts_with("/gb28181/receiver/") || path == "/gb28181/sender" ||
           path.starts_with("/gb28181/sender/") || path == "/rtsp/pull" || path.starts_with("/rtsp/pull/");
}

}    // namespace

bool http_session::control_authorized(const boost::beast::http::request<boost::beast::http::string_body>& request)
{
    // 播放端口需要对浏览器开放，控制接口必须单独鉴权。
    if (config_.control_token.empty())
    {
        boost::system::error_code error;
        const auto peer = stream_.socket().remote_endpoint(error);
        return !error && peer.address().is_loopback();
    }
    const auto authorization = request[boost::beast::http::field::authorization];
    const std::string expected = "Bearer " + config_.control_token;
    return authorization.size() == expected.size() && CRYPTO_memcmp(authorization.data(), expected.data(), expected.size()) == 0;
}

http_session::http_session(worker_context& worker, boost::asio::ip::tcp::socket socket, const config& config)
    : worker_(worker), stream_(std::move(socket)), config_(config)
{
}

void http_session::startup()
{
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void http_session::run(boost::asio::yield_context yield)
{
    boost::beast::flat_buffer buffer;
    boost::beast::http::request<boost::beast::http::string_body> request;
    boost::system::error_code error;

    stream_.expires_after(std::chrono::seconds(30));
    boost::beast::http::async_read(stream_, buffer, request, yield[error]);
    if (error)
    {
        shutdown();
    }
    else
    {
        handle_request(request, yield);
    }
}

void http_session::handle_request(boost::beast::http::request<boost::beast::http::string_body>& request, boost::asio::yield_context yield)
{
    const auto parsed = parse_http_target(request.target());
    if (!parsed)
    {
        send_text_response(request, boost::beast::http::status::bad_request, "bad request target\n", yield);
        return;
    }

    const auto encoded_path = parsed->get_pathname();
    const std::string_view path(encoded_path.data(), encoded_path.size());
    if (path == "/")
    {
        send_text_response(request, boost::beast::http::status::not_found, "not found\n", yield);
        return;
    }
    if (is_control_path(path) && !control_authorized(request))
    {
        // 与控制接口其他错误一致返回 JSON，信令据此识别为明确拒绝。
        boost::beast::http::response<boost::beast::http::string_body> response(boost::beast::http::status::forbidden, request.version());
        response.set(boost::beast::http::field::server, "media_server");
        response.set(boost::beast::http::field::content_type, "application/json");
        response.keep_alive(false);
        response.body() = R"({"error":"forbidden"})";
        response.prepare_payload();
        write_string_response(request, std::move(response), yield);
        return;
    }
    if (path == "/gb28181/receiver" || path.starts_with("/gb28181/receiver/"))
    {
        write_string_response(
            request,
            media_server::handle_gb28181_receiver_request(request, worker_, *parsed, boost::asio::ip::make_address(config_.bind_address)),
            yield);
        return;
    }
    if (path == "/gb28181/sender" || path.starts_with("/gb28181/sender/"))
    {
        write_string_response(
            request,
            media_server::handle_gb28181_sender_request(request, worker_, *parsed, boost::asio::ip::make_address(config_.bind_address)),
            yield);
        return;
    }
    if (path == "/receivers")
    {
        if (request.method() != boost::beast::http::verb::get || parsed->has_search())
        {
            send_text_response(request, boost::beast::http::status::method_not_allowed, "method not allowed\n", yield, "GET");
            return;
        }
        write_string_response(request, make_receiver_list_response(request), yield);
        return;
    }
    if (path == "/receivers/delete")
    {
        auto status = boost::beast::http::status::no_content;
        std::string failure;
        if (request.method() != boost::beast::http::verb::post)
        {
            status = boost::beast::http::status::method_not_allowed;
            failure = "method_not_allowed";
        }
        else if (!boost::beast::iequals(request[boost::beast::http::field::content_type], "application/json"))
        {
            status = boost::beast::http::status::unsupported_media_type;
            failure = "unsupported_media_type";
        }
        else
        {
            boost::system::error_code error;
            const auto value = boost::json::parse(request.body(), error);
            const auto* stream_id = !error && value.is_object() ? value.as_object().if_contains("stream_id") : nullptr;
            if (parsed->has_search() || stream_id == nullptr || value.as_object().size() != 1 || !stream_id->is_string() || stream_id->as_string().empty())
            {
                status = boost::beast::http::status::bad_request;
                failure = "invalid_request";
            }
            else if (const auto session = session_registry::instance().take_receiver_session(std::string_view{stream_id->as_string()}))
            {
                session->shutdown();
            }
            else
            {
                status = boost::beast::http::status::not_found;
                failure = "not_found";
            }
        }
        boost::beast::http::response<boost::beast::http::string_body> response(status, request.version());
        response.set(boost::beast::http::field::server, "media_server");
        if (!failure.empty())
        {
            response.set(boost::beast::http::field::content_type, "application/json");
            response.body() = boost::json::serialize(boost::json::object{{"error", failure}});
        }
        if (status == boost::beast::http::status::method_not_allowed)
        {
            response.set(boost::beast::http::field::allow, "POST");
        }
        response.keep_alive(false);
        response.prepare_payload();
        write_string_response(request, std::move(response), yield);
        return;
    }
    if (path == "/rtsp/pull" || path.starts_with("/rtsp/pull/"))
    {
        write_string_response(request, media_server::handle_rtsp_pull_request(request, worker_, *parsed), yield);
        return;
    }
    if (path == "/play/whep" || path.starts_with("/play/whep/"))
    {
        write_string_response(request, media_server::handle_whep_request(request, worker_, *parsed, config_, stream_.socket(), yield), yield);
        return;
    }
    if (path == "/publish/whip" || path.starts_with("/publish/whip/"))
    {
        write_string_response(request, media_server::handle_whip_request(request, worker_, *parsed, config_, stream_.socket(), yield), yield);
        return;
    }
    if (path == "/play/hls" || path.starts_with("/play/hls/"))
    {
        const auto session = std::make_shared<hls_http_session>(worker_, std::move(stream_), std::move(request), config_);
        session->startup();
        return;
    }

    if (path.ends_with(".flv"))
    {
        const auto session = std::make_shared<http_flv_session>(worker_, std::move(stream_), std::move(request), config_);
        session->startup();
        return;
    }

    if (request.method() != boost::beast::http::verb::get)
    {
        send_text_response(request, boost::beast::http::status::method_not_allowed, "method not allowed\n", yield, "GET");
        return;
    }

    send_text_response(request, boost::beast::http::status::not_found, "not found\n", yield);
}

void http_session::write_string_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                                         boost::beast::http::response<boost::beast::http::string_body> response,
                                         boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (request.method() == boost::beast::http::verb::head)
    {
        boost::beast::http::response_serializer<boost::beast::http::string_body> serializer(response);
        boost::beast::http::async_write_header(stream_, serializer, yield[error]);
    }
    else
    {
        boost::beast::http::async_write(stream_, response, yield[error]);
    }
    if (error && response.result() == boost::beast::http::status::created)
    {
        const auto location = response[boost::beast::http::field::location];
        if (location.starts_with("/play/whep/session/"))
        {
            static_cast<void>(whep::remove(location.substr(std::string_view("/play/whep/session/").size())));
        }
        else if (location.starts_with("/publish/whip/session/"))
        {
            static_cast<void>(whip::remove(location.substr(std::string_view("/publish/whip/session/").size())));
        }
    }
    shutdown();
}

void http_session::send_text_response(boost::beast::http::request<boost::beast::http::string_body>& request,
                                      boost::beast::http::status status,
                                      std::string body,
                                      boost::asio::yield_context yield,
                                      std::string_view allow)
{
    boost::beast::http::response<boost::beast::http::string_body> response(status, request.version());
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, "text/plain");
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();

    write_string_response(request, std::move(response), yield);
}

void http_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void http_session::safe_shutdown()
{
    boost::system::error_code error;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    stream_.socket().close(error);
}

}    // namespace media_server
