#include <iterator>
#include <utility>

#include "media/webrtc/whep.h"
#include "media/http/whep_http.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace
{

constexpr int whep_retry_after_seconds = 1;

whep_http_string_response make_string_response(const whep_http_request& request,
                                               boost::beast::http::status status,
                                               std::string_view content_type,
                                               std::string body,
                                               std::string_view allow = {})
{
    whep_http_string_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, content_type);
    response.set(boost::beast::http::field::access_control_allow_origin, "*");
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
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

whep_http_string_response handle_whep_session_get(const whep_http_request& request, std::string_view session_id)
{
    if (!whep::contains(session_id))
    {
        return make_empty_response(request, boost::beast::http::status::not_found);
    }
    return make_empty_response(request, boost::beast::http::status::no_content);
}

whep_http_string_response handle_whep_post(
    const whep_http_request& request, worker_context& worker, std::string stream_id, const config& application_config)
{
    auto result = whep::create(worker, stream_id, request.body(), application_config);
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
        {
            auto response = make_string_response(request, boost::beast::http::status::conflict, "text/plain", "stream not found\n");
            response.set(boost::beast::http::field::retry_after, std::to_string(whep_retry_after_seconds));
            return response;
        }
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
                                              worker_context& worker,
                                              const boost::urls::url_view& target,
                                              const config& application_config)
{
    const auto segments = target.segments();
    if (segments.size() <= 2)
    {
        return make_string_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n");
    }
    auto first = std::next(segments.begin(), 2);
    const bool session_resource = segments.size() == 4 && *first == "session";
    const bool endpoint_resource = *first != "session";
    if (!session_resource && !endpoint_resource)
    {
        return make_string_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n");
    }

    if (request.method() == boost::beast::http::verb::options)
    {
        return handle_whep_options(request, session_resource);
    }
    if (request.method() == boost::beast::http::verb::get || request.method() == boost::beast::http::verb::head)
    {
        if (session_resource)
        {
            return handle_whep_session_get(request, *std::next(first));
        }
        return make_empty_response(request, boost::beast::http::status::ok, "application/sdp");
    }
    if (request.method() == boost::beast::http::verb::post && endpoint_resource)
    {
        const auto content_type = request[boost::beast::http::field::content_type];
        if (!boost::beast::iequals(content_type, "application/sdp"))
        {
            return make_string_response(request, boost::beast::http::status::unsupported_media_type, "text/plain", "content type must be application/sdp\n");
        }
        std::string stream_id;
        for (auto iterator = first; iterator != segments.end(); ++iterator)
        {
            const auto segment = *iterator;
            if (!stream_id.empty())
            {
                stream_id.push_back('/');
            }
            stream_id.append(segment);
        }
        return handle_whep_post(request, worker, std::move(stream_id), application_config);
    }
    if (request.method() == boost::beast::http::verb::delete_ && session_resource)
    {
        return handle_whep_delete(request, *std::next(first));
    }

    const std::string_view allow = session_resource ? "GET, HEAD, DELETE, OPTIONS" : "GET, HEAD, POST, OPTIONS";
    // 本实现只支持一次完整 SDP POST/answer，不支持 PATCH/Trickle ICE。
    return make_string_response(request, boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", allow);
}

}    // namespace media_server
