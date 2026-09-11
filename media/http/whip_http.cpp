#include <span>
#include <vector>
#include <utility>

#include "media/webrtc/whip.h"
#include "media/net/worker_context.h"
#include "media/http/whip_http.h"

namespace media_server
{
namespace
{

whip_http_string_response make_string_response(const whip_http_request& request,
                                               boost::beast::http::status status,
                                               std::string_view content_type,
                                               std::string body,
                                               std::string_view allow = {})
{
    whip_http_string_response response{status, request.version()};
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

whip_http_string_response make_empty_response(const whip_http_request& request, boost::beast::http::status status)
{
    whip_http_string_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::cache_control, "no-store");
    response.set(boost::beast::http::field::access_control_allow_origin, "*");
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

whip_http_string_response make_error_response(
    const whip_http_request& request, boost::beast::http::status status, std::string body, std::string_view allow = {})
{
    return make_string_response(request, status, "text/plain", std::move(body), allow);
}

whip_http_string_response handle_options(const whip_http_request& request, bool session_resource)
{
    auto response = make_empty_response(request, boost::beast::http::status::ok);
    response.erase(boost::beast::http::field::cache_control);
    response.set(boost::beast::http::field::access_control_allow_methods, session_resource ? "DELETE, OPTIONS" : "POST, OPTIONS");
    response.set(boost::beast::http::field::access_control_allow_headers, "Content-Type");
    if (!session_resource)
    {
        response.set("Accept-Post", "application/sdp");
    }
    return response;
}

whip_http_string_response handle_post(const whip_http_request& request,
                                      worker_context& worker,
                                      std::string stream_name,
                                      const config& application_config)
{
    auto result = whip::create(worker, stream_name, request.body(), application_config);
    switch (result.error)
    {
        case whip::create_error::none:
        {
            auto response = make_string_response(request, boost::beast::http::status::created, "application/sdp", std::move(result.answer_sdp));
            response.set(boost::beast::http::field::location, "/publish/whip/session/" + result.session_id);
            response.set(boost::beast::http::field::cache_control, "no-store");
            response.set(boost::beast::http::field::access_control_expose_headers, "Location");
            return response;
        }
        case whip::create_error::stream_conflict:
            return make_error_response(request, boost::beast::http::status::conflict, "stream already exists\n");
        case whip::create_error::invalid_offer:
            return make_error_response(request, boost::beast::http::status::bad_request, "invalid or unsupported sdp offer\n");
        case whip::create_error::internal_error:
            return make_error_response(request, boost::beast::http::status::internal_server_error, "whip session create failed\n");
    }
    return make_error_response(request, boost::beast::http::status::internal_server_error, "whip session create failed\n");
}

whip_http_string_response handle_delete(const whip_http_request& request, std::string_view session_id)
{
    if (!whip::remove(session_id))
    {
        return make_empty_response(request, boost::beast::http::status::not_found);
    }
    return make_empty_response(request, boost::beast::http::status::no_content);
}

}    // namespace

whip_http_string_response handle_whip_request(const whip_http_request& request,
                                              worker_context& worker,
                                              const boost::urls::url_view& target,
                                              const config& application_config)
{
    std::vector<std::string> path;
    for (const auto segment : target.segments())
    {
        path.emplace_back(segment);
    }
    if (path.size() < 2 || path[0] != "publish" || path[1] != "whip")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not found\n");
    }

    const auto segments = std::span<const std::string>(path).subspan(2);
    const bool session_resource = segments.size() == 2 && segments[0] == "session";
    const bool endpoint_resource = !segments.empty() && segments[0] != "session";
    if (!session_resource && !endpoint_resource)
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not found\n");
    }

    if (request.method() == boost::beast::http::verb::options)
    {
        return handle_options(request, session_resource);
    }

    if (request.method() == boost::beast::http::verb::post && endpoint_resource)
    {
        const auto content_type = request[boost::beast::http::field::content_type];
        if (!boost::beast::iequals(content_type, "application/sdp"))
        {
            return make_error_response(request, boost::beast::http::status::unsupported_media_type, "content type must be application/sdp\n");
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
        return handle_post(request, worker, std::move(stream_name), application_config);
    }

    if (request.method() == boost::beast::http::verb::delete_ && session_resource)
    {
        return handle_delete(request, segments[1]);
    }

    const std::string_view allow = session_resource ? "DELETE, OPTIONS" : "POST, OPTIONS";
    return make_error_response(request, boost::beast::http::status::method_not_allowed, "method not allowed\n", allow);
}

}    // namespace media_server
