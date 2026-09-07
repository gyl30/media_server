#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <string_view>

#include <boost/json.hpp>

#include "media/net/worker_context.h"
#include "media/http/gb28181_http.h"
#include "media/http/gb28181_json.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

namespace media_server
{
namespace
{

constexpr auto tcp_establishment_timeout = std::chrono::seconds(10);

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

gb28181_http_response make_error_response(const gb28181_http_request& request,
                                          boost::beast::http::status status,
                                          std::string_view error,
                                          std::string_view allow = {})
{
    boost::json::object body;
    body["error"] = error;
    return make_json_response(request, status, std::move(body), allow);
}

std::optional<gb28181_http_response> validate_request(const gb28181_http_request& request, const boost::urls::url_view& target)
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

gb28181_http_response handle_receiver_create(const gb28181_http_request& request,
                                             worker_context& worker,
                                             gb28181_receiver_config config,
                                             boost::asio::ip::address bind_address)
{
    const auto stream_name = config.stream_name;
    auto& streams = registry::instance();
    if (streams.find(stream_name))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }

    if (config.transport.mode == gb28181_transport::udp)
    {
        auto session = std::make_shared<gb28181_udp_receiver_session>(worker, stream_name, config.transport, bind_address);
        if (!streams.add_receiver_session(stream_name, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        if (!session->startup())
        {
            streams.remove_receiver_session(stream_name, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }

        const auto local_ports = session->local_ports();
        if (!local_ports)
        {
            streams.remove_receiver_session(stream_name, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        boost::json::object body;
        body["result"] = "ok";
        body["rtp_port"] = local_ports->first;
        body["rtcp_port"] = local_ports->second;
        return make_json_response(request, boost::beast::http::status::created, std::move(body));
    }
    else
    {
        auto session = std::make_shared<gb28181_tcp_receiver_session>(
            worker, stream_name, config.transport, bind_address, tcp_establishment_timeout);
        if (!streams.add_receiver_session(stream_name, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        if (!session->startup())
        {
            streams.remove_receiver_session(stream_name, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
    }

    return make_json_response(request, boost::beast::http::status::created, {{"result", "ok"}});
}

gb28181_http_response handle_sender_create(const gb28181_http_request& request,
                                            worker_context& worker,
                                            gb28181_sender_config config,
                                            boost::asio::ip::address bind_address)
{
    const auto stream_name = config.stream_name;
    const auto sender_id = config.sender_id;
    auto& streams = registry::instance();
    auto stream = streams.find(stream_name);
    if (!stream || !gb28181_rtp_sender::supported_tracks(stream->tracks()))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }

    if (config.transport.mode == gb28181_transport::udp)
    {
        auto session = std::make_shared<gb28181_udp_sender_session>(
            worker, stream, config.transport, std::move(bind_address), sender_id, config.rtcp_enabled);
        if (!streams.add_sender_session(stream_name, sender_id, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        if (!session->startup())
        {
            streams.remove_sender_session(stream_name, sender_id, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
    }
    else
    {
        auto session = std::make_shared<gb28181_tcp_sender_session>(worker,
                                                                    std::weak_ptr<media_stream>{stream},
                                                                    stream_name,
                                                                    sender_id,
                                                                    config.transport,
                                                                    std::move(bind_address),
                                                                    tcp_establishment_timeout);
        if (!streams.add_sender_session(stream_name, sender_id, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        if (!session->startup())
        {
            streams.remove_sender_session(stream_name, sender_id, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
    }

    boost::json::object body;
    body["result"] = "ok";
    return make_json_response(request, boost::beast::http::status::created, std::move(body));
}

}    // namespace

gb28181_http_response handle_gb28181_receiver_request(const gb28181_http_request& request,
                                                   worker_context& worker,
                                                   const boost::urls::url_view& target,
                                                   boost::asio::ip::address bind_address)
{
    const auto path = target.encoded_path();
    if (path != "/gb28181/receiver/create" && path != "/gb28181/receiver/delete")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (const auto error = validate_request(request, target))
    {
        return *error;
    }

    if (path == "/gb28181/receiver/create")
    {
        auto config = parse_gb28181_receiver_config(request.body());
        if (!config)
        {
            return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        return handle_receiver_create(request, worker, std::move(*config), std::move(bind_address));
    }

    const auto stream_name = parse_gb28181_receiver_delete(request.body());
    if (!stream_name)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    auto session = registry::instance().take_receiver_session(*stream_name);
    if (!session)
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    session->shutdown();

    boost::json::object body;
    body["result"] = "ok";
    return make_json_response(request, boost::beast::http::status::ok, std::move(body));
}

gb28181_http_response handle_gb28181_sender_request(const gb28181_http_request& request,
                                                    worker_context& worker,
                                                    const boost::urls::url_view& target,
                                                    boost::asio::ip::address bind_address)
{
    const auto path = target.encoded_path();
    if (path != "/gb28181/sender/create" && path != "/gb28181/sender/delete")
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    if (const auto error = validate_request(request, target))
    {
        return *error;
    }

    if (path == "/gb28181/sender/create")
    {
        auto config = parse_gb28181_sender_config(request.body());
        if (!config)
        {
            return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        return handle_sender_create(request, worker, std::move(*config), std::move(bind_address));
    }

    const auto identity = parse_gb28181_sender_delete(request.body());
    if (!identity)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    auto session = registry::instance().take_sender_session(identity->first, identity->second);
    if (!session)
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    session->shutdown();

    boost::json::object body;
    body["result"] = "ok";
    return make_json_response(request, boost::beast::http::status::ok, std::move(body));
}

}    // namespace media_server
