#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <string_view>

#include <boost/json.hpp>

#include "media/http/gb28181_http.h"
#include "media/http/gb28181_json.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/core/session_registry.h"
#include "media/ps/mpeg_ps_output.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"
#include "media/gb28181/gb28181_udp_sender_session.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"

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

gb28181_http_response make_empty_response(const gb28181_http_request& request, boost::beast::http::status status)
{
    gb28181_http_response response{status, request.version()};
    response.set(boost::beast::http::field::server, "media_server");
    response.keep_alive(false);
    response.prepare_payload();
    return response;
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
    if (stream_registry::instance().find(stream_name))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }

    if (config.transport == gb28181_transport::udp)
    {
        auto session = std::make_shared<gb28181_udp_receiver_session>(worker, stream_name, config.payload_type, config.ssrc);
        if (!session_registry::instance().add_receiver_session(stream_name, config.stream_id, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        const auto local_rtp_port = session->startup(std::move(bind_address));
        if (!local_rtp_port)
        {
            session_registry::instance().remove_receiver_session(stream_name, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }

        boost::json::object body;
        body["rtp_port"] = *local_rtp_port;
        return make_json_response(request, boost::beast::http::status::created, std::move(body));
    }

    auto session = std::make_shared<gb28181_tcp_receiver_session>(worker, stream_name, config.payload_type, config.ssrc);
    if (!session_registry::instance().add_receiver_session(stream_name, config.stream_id, session))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    const bool started = config.transport == gb28181_transport::tcp_active
        ? session->startup(boost::asio::ip::tcp::endpoint{std::move(config.remote_address), config.remote_port})
        : session->startup(std::move(bind_address), config.listen_port);
    if (!started)
    {
        session_registry::instance().remove_receiver_session(stream_name, *session);
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    return make_empty_response(request, boost::beast::http::status::created);
}

gb28181_http_response handle_sender_create(const gb28181_http_request& request,
                                           worker_context& worker,
                                           gb28181_sender_config config,
                                           boost::asio::ip::address bind_address)
{
    const auto stream_name = config.stream_name;
    const auto sender_id = config.sender_id;
    auto stream = stream_registry::instance().find(stream_name);
    if (!stream || !mpeg_ps_output::supported_tracks(stream->tracks()))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }

    if (config.transport == gb28181_transport::udp)
    {
        std::optional<boost::asio::ip::udp::endpoint> remote_rtcp_endpoint;
        if (config.remote_rtcp_port)
        {
            remote_rtcp_endpoint.emplace(config.remote_address, *config.remote_rtcp_port);
        }
        auto session = std::make_shared<gb28181_udp_sender_session>(
            worker,
            stream,
            sender_id,
            boost::asio::ip::udp::endpoint{config.remote_address, config.remote_rtp_port},
            std::move(remote_rtcp_endpoint));
        if (!session_registry::instance().add_sender_session(stream_name, sender_id, config.stream_id, session))
        {
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        if (!session->startup(std::move(bind_address), config.payload_type, config.ssrc))
        {
            session_registry::instance().remove_sender_session(stream_name, sender_id, *session);
            session->shutdown();
            return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
        }
        return make_empty_response(request, boost::beast::http::status::created);
    }

    auto session = std::make_shared<gb28181_tcp_sender_session>(worker, stream, sender_id);
    if (!session_registry::instance().add_sender_session(stream_name, sender_id, config.stream_id, session))
    {
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    if (config.transport == gb28181_transport::tcp_active)
    {
        session->startup(boost::asio::ip::tcp::endpoint{std::move(config.remote_address), config.remote_port}, config.payload_type, config.ssrc);
    }
    else if (!session->startup(std::move(bind_address), config.listen_port, config.payload_type, config.ssrc))
    {
        session_registry::instance().remove_sender_session(stream_name, sender_id, *session);
        return make_error_response(request, boost::beast::http::status::internal_server_error, "operation_failed");
    }
    return make_empty_response(request, boost::beast::http::status::created);
}

}    // namespace

gb28181_http_response handle_gb28181_receiver_request(const gb28181_http_request& request,
                                                      worker_context& worker,
                                                      const boost::urls::url_view& target,
                                                      boost::asio::ip::address bind_address)
{
    const auto path = target.encoded_path();
    if (path != "/gb28181/receiver/create" && path != "/gb28181/receiver/delete" && path != "/gb28181/receiver/update")
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

    if (path == "/gb28181/receiver/update")
    {
        const auto update = parse_gb28181_receiver_update(request.body());
        if (!update)
        {
            return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
        }
        const auto& [identity, ssrc] = *update;
        const auto found = session_registry::instance().find_receiver_session(identity.stream_name, identity.stream_id);
        if (const auto udp = std::dynamic_pointer_cast<gb28181_udp_receiver_session>(found))
        {
            udp->update_ssrc(ssrc);
        }
        else if (const auto tcp = std::dynamic_pointer_cast<gb28181_tcp_receiver_session>(found))
        {
            tcp->update_ssrc(ssrc);
        }
        else
        {
            return make_error_response(request, boost::beast::http::status::not_found, "not_found");
        }
        return make_empty_response(request, boost::beast::http::status::no_content);
    }

    const auto identity = parse_gb28181_receiver_delete(request.body());
    if (!identity)
    {
        return make_error_response(request, boost::beast::http::status::bad_request, "invalid_request");
    }
    auto session = session_registry::instance().take_receiver_session(identity->stream_name, identity->stream_id);
    if (!session)
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    session->shutdown();

    return make_empty_response(request, boost::beast::http::status::no_content);
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
    auto session = session_registry::instance().take_sender_session(identity->stream_name, identity->sender_id, identity->stream_id);
    if (!session)
    {
        return make_error_response(request, boost::beast::http::status::not_found, "not_found");
    }
    session->shutdown();

    return make_empty_response(request, boost::beast::http::status::no_content);
}

}    // namespace media_server
