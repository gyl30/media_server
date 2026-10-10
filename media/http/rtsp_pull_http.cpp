#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <algorithm>
#include <string_view>
#include <initializer_list>
#include <charconv>

#include <boost/json.hpp>

#include "media/http/rtsp_pull_http.h"
#include "media/core/stream_registry.h"
#include "media/core/session_registry.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace media_server
{
namespace
{

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

std::optional<rtsp_pull_config> parse_create_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_id", "url", "username", "password"}))
    {
        return std::nullopt;
    }

    auto stream_id = required_string(*object, "stream_id");
    auto url = required_string(*object, "url");
    std::string username;
    std::string password;
    if (!stream_id || !url ||
        !optional_string(*object, "username", username) || !optional_string(*object, "password", password) ||
        (object->if_contains("password") != nullptr && username.empty()))
    {
        return std::nullopt;
    }
    const auto parsed = ada::parse<ada::url_aggregator>(*url);
    if (!parsed || parsed->get_protocol() != "rtsp:" || parsed->has_credentials())
    {
        return std::nullopt;
    }
    auto host = parsed->get_hostname();
    if (host.empty())
    {
        return std::nullopt;
    }
    std::uint16_t port = 554;
    const auto port_text = parsed->get_port();
    if (!port_text.empty())
    {
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0)
        {
            return std::nullopt;
        }
    }
    if (host.starts_with('[') && host.ends_with(']'))
    {
        host.remove_prefix(1);
        host.remove_suffix(1);
    }
    return rtsp_pull_config{
        .stream_id = std::move(*stream_id),
        .url = std::string(parsed->get_href()),
        .host = std::string(host),
        .port = port,
        .username = std::move(username),
        .password = std::move(password),
    };
}

std::optional<rtsp_pull_http_response> validate_request(const rtsp_pull_http_request& request, const ada::url_aggregator& target)
{
    if (target.has_search())
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

rtsp_pull_http_response handle_create(const rtsp_pull_http_request& request, worker_context& worker, rtsp_pull_config config)
{
    if (stream_registry::instance().find(config.stream_id))
    {
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }

    const auto stream_id = config.stream_id;
    auto session = std::make_shared<rtsp_pull_session>(worker, std::move(config));
    if (!session_registry::instance().add_receiver_session(stream_id, session))
    {
        session->shutdown();
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }
    session->startup();
    return make_empty_response(request, boost::beast::http::status::created);
}

}    // namespace

rtsp_pull_http_response handle_rtsp_pull_request(const rtsp_pull_http_request& request, worker_context& worker, const ada::url_aggregator& target)
{
    const auto path = target.get_pathname();
    if (path != "/rtsp/pull/create")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (const auto error = validate_request(request, target))
    {
        return *error;
    }

    auto config = parse_create_config(request.body());
    if (!config)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    return handle_create(request, worker, std::move(*config));
}

}    // namespace media_server
