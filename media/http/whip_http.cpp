#include <utility>

#include "media/webrtc/whip.h"
#include "media/http/whip_http.h"
#include "media/net/worker_context.h"
#include "media/http/signaling_verify.h"

namespace media_server
{
namespace
{

whip_http_string_response make_string_response(
    const whip_http_request& request, boost::beast::http::status status, std::string_view content_type, std::string body, std::string_view allow = {})
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

whip_http_string_response handle_post(
    const whip_http_request& request,
    worker_context& worker,
    std::string_view stream_id,
    const config& application_config,
    const boost::asio::ip::tcp::socket& socket,
    boost::asio::yield_context yield)
{
    auto result = whip::create(worker, stream_id, request.body(), application_config, socket, yield);
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
            return make_string_response(request, boost::beast::http::status::conflict, "text/plain", "stream already exists\n");
        case whip::create_error::invalid_offer:
            return make_string_response(request, boost::beast::http::status::bad_request, "text/plain", "invalid or unsupported sdp offer\n");
        case whip::create_error::forbidden:
            return make_string_response(request, boost::beast::http::status::forbidden, "text/plain", "stream authorization failed\n");
        case whip::create_error::internal_error:
            return make_string_response(request, boost::beast::http::status::internal_server_error, "text/plain", "whip session create failed\n");
    }
    return make_string_response(request, boost::beast::http::status::internal_server_error, "text/plain", "whip session create failed\n");
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
                                              const ada::url_aggregator& target,
                                              const config& application_config,
                                              const boost::asio::ip::tcp::socket& socket,
                                              boost::asio::yield_context yield)
{
    const std::string_view path(request.target().data(), request.target().size());
    if (!target.get_search().empty() || !target.get_hash().empty() || path.find_first_of("%?#\\ \r\n\t") != std::string_view::npos ||
        path != target.get_pathname())
    {
        return make_string_response(request, boost::beast::http::status::bad_request, "text/plain", "invalid path\n");
    }
    constexpr std::string_view prefix = "/publish/whip/";
    if (!path.starts_with(prefix))
    {
        return make_string_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n");
    }
    const auto resource = path.substr(prefix.size());
    const auto separator = resource.find('/');
    const auto first = resource.substr(0, separator);
    const auto second = separator == std::string_view::npos ? std::string_view{} : resource.substr(separator + 1);
    const bool session_resource = first == "session" && !second.empty() && second.find('/') == std::string_view::npos;
    const bool endpoint_resource = separator == std::string_view::npos && first != "session" && valid_stream_token(first);
    if (!session_resource && !endpoint_resource)
    {
        return make_string_response(request, boost::beast::http::status::not_found, "text/plain", "not found\n");
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
            return make_string_response(request, boost::beast::http::status::unsupported_media_type, "text/plain", "content type must be application/sdp\n");
        }
        return handle_post(request, worker, first, application_config, socket, yield);
    }

    if (request.method() == boost::beast::http::verb::delete_ && session_resource)
    {
        return handle_delete(request, second);
    }

    const std::string_view allow = session_resource ? "DELETE, OPTIONS" : "POST, OPTIONS";
    return make_string_response(request, boost::beast::http::status::method_not_allowed, "text/plain", "method not allowed\n", allow);
}

}    // namespace media_server
