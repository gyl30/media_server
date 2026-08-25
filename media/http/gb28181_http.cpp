#include <optional>
#include <utility>

#include <boost/json.hpp>

#include "media/gb28181/gb28181.h"
#include "media/http/gb28181_http.h"
#include "media/http/gb28181_json.h"

namespace media_server
{
namespace
{

std::optional<gb28181_http_response> validate_request(const gb28181_http_request& request,
                                                      const boost::urls::url_view& target,
                                                      std::span<const std::string> segments)
{
    if (segments.size() != 1 || (segments.front() != "create" && segments.front() != "delete"))
    {
        return make_gb28181_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (!target.params().empty())
    {
        return make_gb28181_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    if (request.method() != boost::beast::http::verb::post)
    {
        return make_gb28181_error_response(
            request, boost::beast::http::status::method_not_allowed, "method_not_allowed", "POST");
    }
    const auto content_type = request[boost::beast::http::field::content_type];
    if (!boost::beast::iequals(content_type, "application/json"))
    {
        return make_gb28181_error_response(request, boost::beast::http::status::unsupported_media_type, "unsupported_media_type");
    }
    return std::nullopt;
}

gb28181_http_response make_json_response(const gb28181_http_request& request,
                                         boost::beast::http::status status,
                                         boost::json::object body,
                                         std::string_view allow = {})
{
    gb28181_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, "application/json");
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    response.keep_alive(false);
    response.body() = boost::json::serialize(body);
    response.prepare_payload();
    return response;
}

gb28181_http_response make_success_response(const gb28181_http_request& request, boost::beast::http::status status)
{
    boost::json::object body;
    body["result"] = "ok";
    return make_json_response(request, status, std::move(body));
}

gb28181_http_response make_operation_failed_response(const gb28181_http_request& request)
{
    boost::json::object body;
    body["error"] = "operation_failed";
    return make_json_response(request, boost::beast::http::status::internal_server_error, std::move(body));
}

}    // namespace

gb28181_http_response handle_gb28181_input_request(const gb28181_http_request& request,
                                                   boost::asio::io_context& owner,
                                                   const boost::urls::url_view& target,
                                                   std::span<const std::string> segments)
{
    if (const auto error = validate_request(request, target, segments))
    {
        return *error;
    }
    if (segments.front() == "create")
    {
        auto config = parse_gb28181_input_config(request.body());
        if (!config)
        {
            return make_gb28181_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        return handle_gb28181_input_create(request, owner, std::move(*config));
    }

    const auto stream_name = parse_gb28181_input_delete(request.body());
    if (!stream_name)
    {
        return make_gb28181_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    return handle_gb28181_input_delete(request, *stream_name);
}

gb28181_http_response handle_gb28181_output_request(const gb28181_http_request& request,
                                                    boost::asio::io_context& owner,
                                                    const boost::urls::url_view& target,
                                                    std::span<const std::string> segments)
{
    if (const auto error = validate_request(request, target, segments))
    {
        return *error;
    }
    if (segments.front() == "create")
    {
        auto config = parse_gb28181_output_config(request.body());
        if (!config)
        {
            return make_gb28181_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        return handle_gb28181_output_create(request, owner, std::move(*config));
    }

    const auto identity = parse_gb28181_output_delete(request.body());
    if (!identity)
    {
        return make_gb28181_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    return handle_gb28181_output_delete(request, identity->first, identity->second);
}

gb28181_http_response make_gb28181_error_response(const gb28181_http_request& request,
                                                  boost::beast::http::status status,
                                                  std::string_view error,
                                                  std::string_view allow)
{
    boost::json::object body;
    body["error"] = error;
    return make_json_response(request, status, std::move(body), allow);
}

gb28181_http_response handle_gb28181_input_create(const gb28181_http_request& request,
                                                  boost::asio::io_context& owner,
                                                  gb28181_input_config config)
{
    if (gb28181::create(owner, std::move(config)) != 0)
    {
        return make_operation_failed_response(request);
    }
    return make_success_response(request, boost::beast::http::status::created);
}

gb28181_http_response handle_gb28181_input_delete(const gb28181_http_request& request, std::string stream_name)
{
    if (gb28181::remove(stream_name) != 0)
    {
        return make_operation_failed_response(request);
    }
    return make_success_response(request, boost::beast::http::status::ok);
}

gb28181_http_response handle_gb28181_output_create(const gb28181_http_request& request,
                                                   boost::asio::io_context& owner,
                                                   gb28181_output_config config)
{
    if (gb28181::create_output(owner, std::move(config)) != 0)
    {
        return make_operation_failed_response(request);
    }
    return make_success_response(request, boost::beast::http::status::created);
}

gb28181_http_response handle_gb28181_output_delete(const gb28181_http_request& request,
                                                   std::string stream_name,
                                                   std::string output_id)
{
    if (gb28181::remove_output(stream_name, output_id) != 0)
    {
        return make_operation_failed_response(request);
    }
    return make_success_response(request, boost::beast::http::status::ok);
}

}    // namespace media_server
