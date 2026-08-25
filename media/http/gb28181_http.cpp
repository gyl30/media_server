#include <optional>
#include <span>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_session_registry.h"
#include "media/gb28181/gb28181_tcp_output_session.h"
#include "media/gb28181/gb28181_tcp_session.h"
#include "media/gb28181/gb28181_udp_output_session.h"
#include "media/gb28181/gb28181_udp_session.h"
#include "media/http/gb28181_http.h"
#include "media/http/gb28181_json.h"
#include "media/net/tcp_acceptor.h"
#include "media/net/tcp_connector.h"

namespace media_server
{
namespace
{

constexpr auto tcp_establishment_timeout = std::chrono::seconds(10);

boost::asio::ip::address bind_address(const boost::asio::ip::address& address)
{
    return address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
}

std::shared_ptr<tcp_socket_source> create_tcp_socket_source(boost::asio::io_context& owner, const gb28181_description& description)
{
    if (description.transport == gb28181_transport::tcp_active)
    {
        return std::make_shared<tcp_connector>(owner.get_executor(),
                                                boost::asio::ip::tcp::endpoint{description.address, description.rtp_port},
                                                tcp_establishment_timeout);
    }
    return std::make_shared<tcp_acceptor>(owner.get_executor(),
                                          description.rtp_port,
                                          bind_address(description.address),
                                          tcp_establishment_timeout);
}

std::vector<std::string> path_segments(const boost::urls::url_view& target)
{
    std::vector<std::string> result;
    for (const auto segment : target.segments())
    {
        result.emplace_back(segment);
    }
    return result;
}

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
                                                   const boost::urls::url_view& target)
{
    const auto path = path_segments(target);
    if (path.empty() || path.front() != "gb28181")
    {
        return make_gb28181_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    const auto segments = std::span<const std::string>(path).subspan(1);
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
                                                    const boost::urls::url_view& target)
{
    const auto path = path_segments(target);
    if (path.size() < 2 || path[0] != "play" || path[1] != "gb28181")
    {
        return make_gb28181_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    const auto segments = std::span<const std::string>(path).subspan(2);
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
    const auto stream_name = config.stream_name;
    if (registry::instance().find(stream_name))
    {
        return make_operation_failed_response(request);
    }

    auto& sessions = gb28181_session_registry::instance();
    if (config.description.transport == gb28181_transport::udp)
    {
        auto concrete = std::make_shared<gb28181_udp_session>(
            owner.get_executor(),
            stream_name,
            config.description,
            gb28181_udp_peer{.rtp = config.remote_rtp_endpoint, .rtcp_port = config.remote_rtcp_port.value_or(0)});
        if (!sessions.add_input(stream_name, concrete))
        {
            return make_operation_failed_response(request);
        }
        if (!concrete->startup())
        {
            sessions.remove_input(stream_name, *concrete);
            concrete->shutdown();
            return make_operation_failed_response(request);
        }
    }
    else
    {
        auto source = create_tcp_socket_source(owner, config.description);
        auto concrete = std::make_shared<gb28181_tcp_session>(owner.get_executor(),
                                                              std::move(source),
                                                              stream_name,
                                                              config.description.payload_type,
                                                              config.description.ssrc);
        if (!sessions.add_input(stream_name, concrete))
        {
            return make_operation_failed_response(request);
        }
        if (!concrete->startup())
        {
            sessions.remove_input(stream_name, *concrete);
            concrete->shutdown();
            return make_operation_failed_response(request);
        }
    }
    return make_success_response(request, boost::beast::http::status::created);
}

gb28181_http_response handle_gb28181_input_delete(const gb28181_http_request& request, std::string stream_name)
{
    auto session = gb28181_session_registry::instance().take_input(stream_name);
    if (!session)
    {
        return make_operation_failed_response(request);
    }
    session->shutdown();
    return make_success_response(request, boost::beast::http::status::ok);
}

gb28181_http_response handle_gb28181_output_create(const gb28181_http_request& request,
                                                   boost::asio::io_context& owner,
                                                   gb28181_output_config config)
{
    const auto stream_name = config.stream_name;
    const auto output_id = config.output_id;
    auto stream = registry::instance().find(stream_name);
    if (!stream || !gb28181_output_media::supported_tracks(stream->tracks()))
    {
        return make_operation_failed_response(request);
    }

    auto& sessions = gb28181_session_registry::instance();
    if (config.description.transport == gb28181_transport::udp)
    {
        auto concrete = std::make_shared<gb28181_udp_output_session>(
            owner.get_executor(), stream, config.description, output_id, config.rtcp);
        if (!sessions.add_output(stream_name, output_id, concrete))
        {
            return make_operation_failed_response(request);
        }
        if (!concrete->startup())
        {
            sessions.remove_output(stream_name, output_id, *concrete);
            concrete->shutdown();
            return make_operation_failed_response(request);
        }
    }
    else
    {
        auto source = create_tcp_socket_source(owner, config.description);
        auto concrete = std::make_shared<gb28181_tcp_output_session>(owner.get_executor(),
                                                                      std::move(source),
                                                                      std::weak_ptr<media_stream>{stream},
                                                                      stream_name,
                                                                      output_id,
                                                                      config.description.payload_type,
                                                                      config.description.ssrc);
        if (!sessions.add_output(stream_name, output_id, concrete))
        {
            return make_operation_failed_response(request);
        }
        if (!concrete->startup())
        {
            sessions.remove_output(stream_name, output_id, *concrete);
            concrete->shutdown();
            return make_operation_failed_response(request);
        }
    }
    return make_success_response(request, boost::beast::http::status::created);
}

gb28181_http_response handle_gb28181_output_delete(const gb28181_http_request& request,
                                                   std::string stream_name,
                                                   std::string output_id)
{
    auto session = gb28181_session_registry::instance().take_output(stream_name, output_id);
    if (!session)
    {
        return make_operation_failed_response(request);
    }
    session->shutdown();
    return make_success_response(request, boost::beast::http::status::ok);
}

}    // namespace media_server
