#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <algorithm>
#include <string_view>
#include <initializer_list>

#include <boost/json.hpp>

#include "media/core/stream_id.h"
#include "media/http/rtsp_pull_http.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace media_server
{
namespace
{

struct rtsp_pull_create_config
{
    std::string stream_id;
    std::string source_id;
    std::string stream_name;
    std::string url;
    std::string username;
    std::string password;
};

struct rtsp_pull_identity
{
    std::string stream_id;
    std::string stream_name;
};

rtsp_pull_http_response make_json_response(const rtsp_pull_http_request& request,
                                           boost::beast::http::status status,
                                           boost::json::object body,
                                           std::string_view allow = {})
{
    rtsp_pull_http_response response{status, request.version()};
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

rtsp_pull_http_response make_error_response(const rtsp_pull_http_request& request,
                                            boost::beast::http::status status,
                                            std::string_view error,
                                            std::string_view allow = {})
{
    boost::json::object body;
    body["error"] = error;
    return make_json_response(request, status, std::move(body), allow);
}

rtsp_pull_http_response make_empty_response(const rtsp_pull_http_request& request, boost::beast::http::status status)
{
    rtsp_pull_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

std::optional<boost::json::object> parse_object(std::string_view body)
{
    boost::system::error_code error;
    auto value = boost::json::parse(body, error);
    if (error || !value.is_object())
    {
        return std::nullopt;
    }
    return value.as_object();
}

bool has_only_fields(const boost::json::object& object, std::initializer_list<std::string_view> fields)
{
    return std::ranges::all_of(
        object, [fields](const auto& member) { return std::find(fields.begin(), fields.end(), std::string_view{member.key()}) != fields.end(); });
}

std::optional<std::string> required_string(const boost::json::object& object, std::string_view key)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty())
    {
        return std::nullopt;
    }
    return std::string{value->as_string()};
}

bool optional_string(const boost::json::object& object, std::string_view key, std::string& result)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
    {
        return true;
    }
    if (!value->is_string())
    {
        return false;
    }
    result = std::string{value->as_string()};
    return true;
}

std::optional<rtsp_pull_create_config> parse_create_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_id", "source_id", "stream_name", "url", "username", "password"}))
    {
        return std::nullopt;
    }

    auto stream_id = required_string(*object, "stream_id");
    auto source_id = required_string(*object, "source_id");
    auto stream_name = required_string(*object, "stream_name");
    auto url = required_string(*object, "url");
    std::string username;
    std::string password;
    if (!stream_id || !valid_stream_id(*stream_id) || !source_id || !valid_stream_id(*source_id) || !stream_name || !url ||
        !optional_string(*object, "username", username) || !optional_string(*object, "password", password) ||
        (object->if_contains("password") != nullptr && username.empty()))
    {
        return std::nullopt;
    }
    return rtsp_pull_create_config{
        .stream_id = std::move(*stream_id),
        .source_id = std::move(*source_id),
        .stream_name = std::move(*stream_name),
        .url = std::move(*url),
        .username = std::move(username),
        .password = std::move(password),
    };
}

std::optional<rtsp_pull_identity> parse_delete_identity(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_id", "stream_name"}))
    {
        return std::nullopt;
    }
    auto stream_id = required_string(*object, "stream_id");
    auto stream_name = required_string(*object, "stream_name");
    if (!stream_id || !valid_stream_id(*stream_id) || !stream_name)
    {
        return std::nullopt;
    }
    return rtsp_pull_identity{.stream_id = std::move(*stream_id), .stream_name = std::move(*stream_name)};
}

std::optional<rtsp_pull_http_response> validate_request(const rtsp_pull_http_request& request, const boost::urls::url_view& target)
{
    if (!target.params().empty())
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    if (request.method() != boost::beast::http::verb::post)
    {
        return make_error_response(request, boost::beast::http::status::method_not_allowed, "method_not_allowed", "POST");
    }
    if (!boost::beast::iequals(request[boost::beast::http::field::content_type], "application/json"))
    {
        return make_error_response(request, boost::beast::http::status::unsupported_media_type, "unsupported_media_type");
    }
    return std::nullopt;
}

rtsp_pull_http_response handle_create(const rtsp_pull_http_request& request, worker_context& worker, rtsp_pull_create_config config)
{
    if (!rtsp_pull_session::valid_url(config.url))
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }

    auto& streams = stream_registry::instance();
    if (streams.find(config.stream_name))
    {
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }

    const auto stream_name = config.stream_name;
    auto session = std::make_shared<rtsp_pull_session>(worker,
                                                       std::move(config.stream_id),
                                                       std::move(config.source_id),
                                                       stream_name,
                                                       std::move(config.url),
                                                       std::move(config.username),
                                                       std::move(config.password),
                                                       std::chrono::milliseconds{15'000},
                                                       std::chrono::milliseconds{15'000},
                                                       1024U * 1024U);
    if (!streams.add_receiver_session(stream_name, session))
    {
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }
    if (!session->startup())
    {
        streams.remove_receiver_session(stream_name, *session);
        session->shutdown();
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    return make_empty_response(request, boost::beast::http::status::created);
}

}    // namespace

rtsp_pull_http_response handle_rtsp_pull_request(const rtsp_pull_http_request& request, worker_context& worker, const boost::urls::url_view& target)
{
    const auto path = target.encoded_path();
    if (path != "/rtsp/pull/create" && path != "/rtsp/pull/delete")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (const auto error = validate_request(request, target))
    {
        return *error;
    }

    if (path == "/rtsp/pull/create")
    {
        auto config = parse_create_config(request.body());
        if (!config)
        {
            return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        return handle_create(request, worker, std::move(*config));
    }

    const auto identity = parse_delete_identity(request.body());
    if (!identity)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    auto session = stream_registry::instance().take_receiver_session_as<rtsp_pull_session>(identity->stream_name, identity->stream_id);
    if (!session)
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    session->shutdown();
    return make_empty_response(request, boost::beast::http::status::no_content);
}

}    // namespace media_server
