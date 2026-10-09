#include <string>
#include <utility>
#include <optional>

#include <boost/json.hpp>

#include "media/http/http_async.h"
#include "media/http/receiver_http.h"
#include "media/core/session_registry.h"

namespace media_server
{
namespace
{

receiver_http_response make_json_response(const receiver_http_request& request, boost::beast::http::status status, const boost::json::object& body)
{
    receiver_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, "application/json");
    response.keep_alive(false);
    response.body() = boost::json::serialize(body);
    response.prepare_payload();
    return response;
}

receiver_http_response make_error_response(const receiver_http_request& request, boost::beast::http::status status, std::string_view error)
{
    return make_json_response(request, status, boost::json::object{{"error", error}});
}

std::optional<receiver_identity> parse_identity(std::string_view body)
{
    boost::system::error_code error;
    const auto value = boost::json::parse(body, error);
    if (error || !value.is_object() || value.as_object().size() != 2)
    {
        return std::nullopt;
    }
    const auto& object = value.as_object();
    const auto* stream_name = object.if_contains("stream_name");
    const auto* stream_id = object.if_contains("stream_id");
    if (stream_name == nullptr || stream_id == nullptr || !stream_name->is_string() || !stream_id->is_string() ||
        stream_name->as_string().empty() || stream_id->as_string().empty())
    {
        return std::nullopt;
    }
    return receiver_identity{.stream_name = std::string(stream_name->as_string()), .stream_id = std::string(stream_id->as_string())};
}

}    // namespace

async_result close_receiver(std::string_view stream_name, std::string_view stream_id, boost::asio::yield_context yield)
{
    return await_result(
        [stream_name = std::string(stream_name), stream_id = std::string(stream_id)](auto done)
        {
            const auto session = session_registry::instance().begin_close_receiver(
                stream_name,
                stream_id,
                [done](receiver_close reason) { done(reason == receiver_close::completed ? async_result::completed : async_result::stopped); });
            if (!session)
            {
                done(async_result::not_found);
                return;
            }
            session->shutdown();
        },
        yield);
}

receiver_http_response handle_receiver_request(const receiver_http_request& request, const boost::urls::url_view& target, boost::asio::yield_context yield)
{
    const auto path = target.encoded_path();
    if (!target.params().empty())
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    if (path == "/receivers")
    {
        if (request.method() != boost::beast::http::verb::get)
        {
            return make_error_response(request, boost::beast::http::status::method_not_allowed, "method_not_allowed");
        }
        boost::json::array receivers;
        for (const auto& receiver : session_registry::instance().receivers())
        {
            receivers.push_back(boost::json::object{{"stream_name", receiver.stream_name}, {"stream_id", receiver.stream_id}});
        }
        return make_json_response(request, boost::beast::http::status::ok, boost::json::object{{"receivers", std::move(receivers)}});
    }
    if (path != "/receivers/delete")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (request.method() != boost::beast::http::verb::post)
    {
        return make_error_response(request, boost::beast::http::status::method_not_allowed, "method_not_allowed");
    }
    if (!boost::beast::iequals(request[boost::beast::http::field::content_type], "application/json"))
    {
        return make_error_response(request, boost::beast::http::status::unsupported_media_type, "unsupported_media_type");
    }
    const auto identity = parse_identity(request.body());
    if (!identity)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    const auto result = close_receiver(identity->stream_name, identity->stream_id, yield);
    if (result == async_result::not_found)
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (result == async_result::stopped)
    {
        return make_error_response(request, boost::beast::http::status::service_unavailable, "service_unavailable");
    }
    receiver_http_response response{boost::beast::http::status::no_content, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

}    // namespace media_server
