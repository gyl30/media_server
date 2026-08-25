#include <utility>

#include <boost/json.hpp>

#include "media/gb28181/gb28181.h"
#include "media/http/gb28181_http.h"

namespace media_server
{
namespace
{

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
    response.keep_alive(request.keep_alive());
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
