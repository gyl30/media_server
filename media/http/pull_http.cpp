#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <charconv>
#include <algorithm>
#include <string_view>
#include <initializer_list>

#include <simdjson.h>

#include "config.h"
#include "media/http/pull_http.h"
#include "media/core/stream_registry.h"
#include "media/core/session_registry.h"
#include "media/rtsp/rtsp_pull_session.h"
#include "media/rtmp/rtmp_pull_session.h"
#include "media/webrtc/whep_pull_session.h"

namespace media_server
{
namespace
{

pull_http_response make_json_response(const pull_http_request& request,
                                      boost::beast::http::status status,
                                      std::string body,
                                      std::string_view allow = {})
{
    pull_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.set(boost::beast::http::field::content_type, "application/json");
    if (!allow.empty())
    {
        response.set(boost::beast::http::field::allow, allow);
    }
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

pull_http_response make_error_response(const pull_http_request& request,
                                       boost::beast::http::status status,
                                       std::string_view error,
                                       std::string_view allow = {})
{
    simdjson::builder::string_builder body;
    body.start_object();
    body.append_key_value("error", error);
    body.end_object();
    return make_json_response(request, status, std::string{body.view().value_unsafe()}, allow);
}

pull_http_response make_empty_response(const pull_http_request& request, boost::beast::http::status status)
{
    pull_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

std::optional<simdjson::dom::object> parse_object(simdjson::dom::parser& parser, std::string_view body)
{
    simdjson::dom::object object;
    if (parser.parse(body.data(), body.size()).get(object) != simdjson::SUCCESS)
    {
        return std::nullopt;
    }
    return object;
}

bool has_only_fields(const simdjson::dom::object& object, std::initializer_list<std::string_view> fields)
{
    std::uint32_t seen = 0;
    for (const auto member : object)
    {
        const auto position = std::find(fields.begin(), fields.end(), member.key);
        if (position == fields.end())
        {
            return false;
        }
        const auto bit = 1U << static_cast<unsigned int>(position - fields.begin());
        if ((seen & bit) != 0)
        {
            return false;
        }
        seen |= bit;
    }
    return true;
}

bool has_field(const simdjson::dom::object& object, std::string_view key) { return object.at_key(key).error() != simdjson::NO_SUCH_FIELD; }

std::optional<std::string> required_string(const simdjson::dom::object& object, std::string_view key)
{
    std::string_view value;
    if (object.at_key(key).get_string().get(value) != simdjson::SUCCESS || value.empty())
    {
        return std::nullopt;
    }
    return std::string{value};
}

bool optional_string(const simdjson::dom::object& object, std::string_view key, std::string& result)
{
    const auto value = object.at_key(key);
    if (value.error() == simdjson::NO_SUCH_FIELD)
    {
        return true;
    }
    std::string_view text;
    if (value.get_string().get(text) != simdjson::SUCCESS)
    {
        return false;
    }
    result = std::string{text};
    return true;
}

bool parse_host_port(const ada::url_aggregator& url, std::string& host, std::uint16_t& port)
{
    auto hostname = url.get_hostname();
    if (hostname.empty())
    {
        return false;
    }
    const auto port_text = url.get_port();
    if (!port_text.empty())
    {
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0)
        {
            return false;
        }
    }
    if (hostname.starts_with('[') && hostname.ends_with(']'))
    {
        hostname.remove_prefix(1);
        hostname.remove_suffix(1);
    }
    host = std::string(hostname);
    return true;
}

std::optional<rtsp_pull_config> parse_rtsp_config(std::string stream_id, const ada::url_aggregator& url, std::string username, std::string password)
{
    rtsp_pull_config config{
        .stream_id = std::move(stream_id),
        .url = std::string(url.get_href()),
        .host = {},
        .port = 554,
        .username = std::move(username),
        .password = std::move(password),
    };
    if (!parse_host_port(url, config.host, config.port))
    {
        return std::nullopt;
    }
    return config;
}

std::optional<rtmp_pull_config> parse_rtmp_config(std::string stream_id, const ada::url_aggregator& url)
{
    rtmp_pull_config config{.stream_id = std::move(stream_id), .host = {}, .port = 1935, .app = {}, .stream = {}, .tc_url = {}};
    if (!parse_host_port(url, config.host, config.port))
    {
        return std::nullopt;
    }
    const auto path = url.get_pathname();
    const auto separator = path.find('/', 1);
    if (path.size() < 2 || separator == std::string_view::npos || separator == 1 || separator + 1 == path.size())
    {
        return std::nullopt;
    }
    config.app = std::string(path.substr(1, separator - 1));
    config.stream = std::string(path.substr(separator + 1)) + std::string(url.get_search());
    config.tc_url = std::string(url.get_protocol()) + "//" + std::string(url.get_host()) + "/" + config.app;
    return config;
}

template <typename session_type>
pull_http_response start_session(const pull_http_request& request, const std::string& stream_id, std::shared_ptr<session_type> session)
{
    if (stream_registry::instance().find(stream_id))
    {
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }
    if (!session_registry::instance().add_receiver_session(stream_id, session))
    {
        session->shutdown();
        return make_error_response(request, boost::beast::http::status::conflict, "conflict");
    }
    session->startup();
    return make_empty_response(request, boost::beast::http::status::created);
}

pull_http_response handle_create(const pull_http_request& request, worker_context& worker, const config& application_config)
{
    const auto invalid = [&request]() { return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request"); };
    simdjson::dom::parser parser;
    const auto object = parse_object(parser, request.body());
    if (!object || !has_only_fields(*object, {"stream_id", "url", "username", "password", "token"}))
    {
        return invalid();
    }
    auto stream_id = required_string(*object, "stream_id");
    const auto url_text = required_string(*object, "url");
    std::string username;
    std::string password;
    std::string token;
    if (!stream_id || !url_text || !optional_string(*object, "username", username) || !optional_string(*object, "password", password) ||
        !optional_string(*object, "token", token))
    {
        return invalid();
    }
    const auto url = ada::parse<ada::url_aggregator>(*url_text);
    if (!url || url->has_credentials())
    {
        return invalid();
    }

    const auto protocol = url->get_protocol();
    if (protocol != "http:" && has_field(*object, "token"))
    {
        return invalid();
    }
    if (protocol == "rtsp:")
    {
        if (has_field(*object, "password") && username.empty())
        {
            return invalid();
        }
        auto config = parse_rtsp_config(*stream_id, *url, std::move(username), std::move(password));
        if (!config)
        {
            return invalid();
        }
        return start_session(request, *stream_id, std::make_shared<rtsp_pull_session>(worker, std::move(*config)));
    }
    if (protocol == "rtmp:" && !has_field(*object, "username") && !has_field(*object, "password"))
    {
        auto config = parse_rtmp_config(*stream_id, *url);
        if (!config)
        {
            return invalid();
        }
        return start_session(request, *stream_id, std::make_shared<rtmp_pull_session>(worker, std::move(*config)));
    }
    if (protocol == "http:" && !has_field(*object, "username") && !has_field(*object, "password") && !url->get_hostname().empty())
    {
        std::string host;
        std::uint16_t port = 80;
        boost::system::error_code error;
        const auto local_address = boost::asio::ip::make_address(application_config.webrtc_address, error);
        if (!parse_host_port(*url, host, port) || error || local_address.is_unspecified())
        {
            return invalid();
        }
        return start_session(
            request,
            *stream_id,
            whep_pull::create(whep_pull_config{
                .stream_id = *stream_id, .url = std::string(url->get_href()), .token = std::move(token), .local_address = local_address}));
    }
    return invalid();
}

std::optional<pull_http_response> validate_request(const pull_http_request& request, const ada::url_aggregator& target)
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

}    // namespace

pull_http_response handle_pull_request(const pull_http_request& request,
                                       worker_context& worker,
                                       const config& application_config,
                                       const ada::url_aggregator& target)
{
    const auto path = target.get_pathname();
    if (path != "/pull/create")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (const auto error = validate_request(request, target))
    {
        return *error;
    }
    return handle_create(request, worker, application_config);
}

}    // namespace media_server
