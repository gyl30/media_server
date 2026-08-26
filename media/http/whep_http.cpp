#include <span>
#include <vector>
#include <utility>

#include "media/webrtc/whep.h"
#include "media/http/whep_http.h"

namespace media_server
{
namespace
{

constexpr int whep_retry_after_seconds = 1;

whep_http_string_response make_string_response(const whep_http_request& request,
                                               boost::beast::http::status status,
                                               std::string_view content_type,
                                               std::string body,
                                               std::string_view allow = {},
                                               int retry_after_seconds = 0)
{
    whep_http_string_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, content_type);
    response.set(boost::beast::http::field::access_control_allow_origin, "*");
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    if (retry_after_seconds > 0)
    {
        response.set(boost::beast::http::field::retry_after, std::to_string(retry_after_seconds));
    }
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

whep_http_string_response make_empty_response(const whep_http_request& request, boost::beast::http::status status, std::string_view content_type = {})
{
    whep_http_string_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::cache_control, "no-store");
    response.set(boost::beast::http::field::access_control_allow_origin, "*");
    if (!content_type.empty())
    {
        response.set(boost::beast::http::field::content_type, content_type);
    }
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

whep_http_string_response make_whep_error_response(
    const whep_http_request& request, boost::beast::http::status status, std::string body, int retry_after_seconds = 0, std::string_view allow = {})
{
    return make_string_response(request, status, "text/plain", std::move(body), allow, retry_after_seconds);
}

whep_http_string_response handle_whep_options(const whep_http_request& request, bool session_resource)
{
    auto response = make_empty_response(request, boost::beast::http::status::ok);
    response.erase(boost::beast::http::field::cache_control);
    response.set(boost::beast::http::field::access_control_allow_methods,
                 session_resource ? "GET, HEAD, DELETE, OPTIONS" : "GET, HEAD, POST, OPTIONS");
    response.set(boost::beast::http::field::access_control_allow_headers, "Content-Type");
    if (!session_resource)
    {
        response.set("Accept-Post", "application/sdp");
    }
    return response;
}

whep_http_string_response handle_whep_endpoint_get(const whep_http_request& request)
{
    return make_empty_response(request, boost::beast::http::status::ok, "application/sdp");
}

whep_http_string_response handle_whep_session_get(const whep_http_request& request, std::string_view session_id)
{
    if (!whep::contains(session_id))
    {
        return make_empty_response(request, boost::beast::http::status::not_found);
    }
    return make_empty_response(request, boost::beast::http::status::no_content);
}

whep_http_string_response handle_whep_post(const whep_http_request& request,
                                           boost::asio::any_io_executor executor,
                                           std::string stream_name,
                                           const config& application_config)
{
    auto result = whep::create(std::move(executor), stream_name, request.body(), application_config);
    switch (result.error)
    {
        case whep::create_error::none:
        {
            auto response = make_string_response(request, boost::beast::http::status::created, "application/sdp", std::move(result.answer_sdp));
            response.set(boost::beast::http::field::location, "/play/whep/session/" + result.session_id);
            response.set(boost::beast::http::field::cache_control, "no-store");
            response.set(boost::beast::http::field::access_control_expose_headers, "Location");
            return response;
        }
        case whep::create_error::stream_not_found:
            return make_string_response(
                request, boost::beast::http::status::conflict, "text/plain", "stream not found\n", {}, whep_retry_after_seconds);
        case whep::create_error::invalid_offer:
            return make_string_response(request, boost::beast::http::status::bad_request, "text/plain", "invalid or unsupported sdp offer\n");
        case whep::create_error::internal_error:
            return make_string_response(request, boost::beast::http::status::internal_server_error, "text/plain", "whep session create failed\n");
    }
    return make_string_response(request, boost::beast::http::status::internal_server_error, "text/plain", "whep session create failed\n");
}

whep_http_string_response handle_whep_delete(const whep_http_request& request, std::string_view session_id)
{
    if (!whep::remove(session_id))
    {
        return make_empty_response(request, boost::beast::http::status::not_found);
    }
    return make_empty_response(request, boost::beast::http::status::no_content);
}

}    // namespace

whep_http_string_response handle_whep_request(const whep_http_request& request,
                                              boost::asio::any_io_executor executor,
                                              const boost::urls::url_view& target,
                                              const config& application_config)
{
    std::vector<std::string> path;
    for (const auto segment : target.segments())
    {
        path.emplace_back(segment);
    }
    if (path.size() < 2 || path[0] != "play" || path[1] != "whep")
    {
        return make_whep_error_response(request, boost::beast::http::status::not_found, "not found\n");
    }
    const auto segments = std::span<const std::string>(path).subspan(2);
    const bool session_resource = segments.size() == 2 && segments[0] == "session";
    const bool endpoint_resource = !segments.empty() && segments[0] != "session";
    if (!session_resource && !endpoint_resource)
    {
        return make_whep_error_response(request, boost::beast::http::status::not_found, "not found\n");
    }

    if (request.method() == boost::beast::http::verb::options)
    {
        return handle_whep_options(request, session_resource);
    }
    if (request.method() == boost::beast::http::verb::get || request.method() == boost::beast::http::verb::head)
    {
        if (session_resource)
        {
            return handle_whep_session_get(request, segments[1]);
        }
        return handle_whep_endpoint_get(request);
    }
    if (request.method() == boost::beast::http::verb::post && endpoint_resource)
    {
        const auto content_type = request[boost::beast::http::field::content_type];
        if (!boost::beast::iequals(content_type, "application/sdp"))
        {
            return make_whep_error_response(request, boost::beast::http::status::unsupported_media_type, "content type must be application/sdp\n");
        }
        std::string stream_name;
        for (const auto& segment : segments)
        {
            if (!stream_name.empty())
            {
                stream_name.push_back('/');
            }
            stream_name.append(segment);
        }
        return handle_whep_post(request, std::move(executor), std::move(stream_name), application_config);
    }
    if (request.method() == boost::beast::http::verb::delete_ && session_resource)
    {
        return handle_whep_delete(request, segments[1]);
    }

    const std::string_view allow = session_resource ? "GET, HEAD, DELETE, OPTIONS" : "GET, HEAD, POST, OPTIONS";
    // 本实现只支持一次完整 SDP POST/answer，不支持 PATCH/Trickle ICE。
    return make_whep_error_response(request, boost::beast::http::status::method_not_allowed, "method not allowed\n", 0, allow);
}

}    // namespace media_server
