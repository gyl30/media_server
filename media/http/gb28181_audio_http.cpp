#include <limits>
#include <utility>

#include <boost/json.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "media/http/signaling_verify.h"
#include "media/http/gb28181_audio_http.h"
#include "media/gb28181/gb28181_audio_session.h"

namespace media_server
{
namespace
{
namespace http = boost::beast::http;
using request_type = http::request<http::string_body>;
using response_type = http::response<http::string_body>;

response_type response(const request_type& request, http::status status, std::string_view error = {})
{
    response_type result(status, request.version());
    result.set(http::field::server, "media_server");
    result.keep_alive(false);
    if (!error.empty())
    {
        result.set(http::field::content_type, "application/json");
        result.body() = boost::json::serialize(boost::json::object{{"error", error}});
    }
    result.prepare_payload();
    return result;
}
}    // namespace

response_type handle_gb28181_audio_request(const request_type& request,
                                           worker_context& worker,
                                           const ada::url_aggregator& target,
                                           boost::asio::ip::address bind_address,
                                           boost::asio::yield_context yield)
{
    const auto path = target.get_pathname();
    if (path != "/gb28181/audio/create" && path != "/gb28181/audio/configure" && path != "/gb28181/audio/status" && path != "/gb28181/audio/delete")
    {
        return response(request, http::status::not_found, "not_found");
    }
    if (request.method() != http::verb::post)
    {
        auto result = response(request, http::status::method_not_allowed, "method_not_allowed");
        result.set(http::field::allow, "POST");
        return result;
    }
    if (!boost::beast::iequals(request[http::field::content_type], "application/json"))
    {
        return response(request, http::status::unsupported_media_type, "unsupported_media_type");
    }
    boost::system::error_code error;
    const auto value = boost::json::parse(request.body(), error);
    if (target.has_search() || error || !value.is_object())
    {
        return response(request, http::status::bad_request, "invalid_request");
    }
    const auto& object = value.as_object();
    const auto* identity = object.if_contains("session_id");
    const bool configuring = path == "/gb28181/audio/configure";
    if (!identity || !identity->is_string() || !valid_stream_token(identity->as_string()) || object.size() != (configuring ? 6U : 1U))
    {
        return response(request, http::status::bad_request, "invalid_request");
    }
    const std::string id(identity->as_string());
    if (path == "/gb28181/audio/create")
    {
        if (!bind_address.is_v4())
        {
            return response(request, http::status::bad_request, "unsupported_address");
        }
        auto session = std::make_shared<gb28181_audio_session>(worker, id);
        if (!session_registry::instance().add_sender_session(id, id, session))
        {
            return response(request, http::status::conflict, "session_conflict");
        }
        const auto port = session->startup(std::move(bind_address));
        if (!port)
        {
            session_registry::instance().remove_sender_session(id, id, *session);
            session->shutdown();
            return response(request, http::status::internal_server_error, "operation_failed");
        }
        auto result = response(request, http::status::created);
        result.set(http::field::content_type, "application/json");
        result.set(http::field::location, "/gb28181/audio/session/" + id);
        result.body() = boost::json::serialize(boost::json::object{{"rtp_port", *port}});
        result.prepare_payload();
        return result;
    }

    std::string token;
    boost::asio::ip::udp::endpoint rtp;
    boost::asio::ip::udp::endpoint rtcp;
    std::uint32_t ssrc{};
    if (configuring)
    {
        const auto* ticket = object.if_contains("upload_token");
        const auto* address = object.if_contains("remote_address");
        if (!ticket || !ticket->is_string() || !valid_stream_token(ticket->as_string()) || !address || !address->is_string())
        {
            return response(request, http::status::bad_request, "invalid_request");
        }
        auto remote = boost::asio::ip::make_address(address->as_string(), error);
        if (error || !remote.is_v4() || remote.is_unspecified() || remote.is_multicast())
        {
            return response(request, http::status::bad_request, "invalid_request");
        }
        const auto number = [&object](std::string_view key, std::uint64_t maximum) -> std::optional<std::uint32_t>
        {
            const auto* item = object.if_contains(key);
            if (!item || (!item->is_int64() && !item->is_uint64()))
            {
                return std::nullopt;
            }
            boost::system::error_code conversion;
            const auto integer = item->to_number<std::uint64_t>(conversion);
            if (conversion || integer == 0 || integer > maximum)
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(integer);
        };
        const auto rtp_port = number("rtp_port", 65535);
        const auto rtcp_port = number("rtcp_port", 65535);
        const auto source = number("ssrc", std::numeric_limits<std::uint32_t>::max());
        if (!rtp_port || !rtcp_port || !source || *rtp_port == *rtcp_port)
        {
            return response(request, http::status::bad_request, "invalid_request");
        }
        rtp = {remote, static_cast<std::uint16_t>(*rtp_port)};
        rtcp = {remote, static_cast<std::uint16_t>(*rtcp_port)};
        ssrc = *source;
        token = ticket->as_string();
    }
    const auto session = std::dynamic_pointer_cast<gb28181_audio_session>(session_registry::instance().find_sender_session(id, id));
    if (!session)
    {
        return response(request, http::status::not_found, "not_found");
    }
    if (configuring)
    {
        const bool configured = boost::asio::async_initiate<boost::asio::yield_context, void(bool)>(
            [session, rtp, rtcp, ssrc, token = std::move(token)](auto completion) mutable
            {
                const auto executor = boost::asio::get_associated_executor(completion);
                auto work = boost::asio::make_work_guard(session->worker().io());
                if (session->worker().stop_requested())
                {
                    boost::asio::post(executor, [completion = std::move(completion)]() mutable { completion(false); });
                    return;
                }
                boost::asio::post(session->worker().io(),
                                  [session,
                                   rtp,
                                   rtcp,
                                   ssrc,
                                   token = std::move(token),
                                   completion = std::move(completion),
                                   executor,
                                   work = std::move(work)]() mutable
                                  {
                                      const bool accepted = session->configure(rtp, rtcp, ssrc, std::move(token));
                                      boost::asio::post(executor, [completion = std::move(completion), accepted]() mutable { completion(accepted); });
                                  });
            },
            yield);
        return response(request, configured ? http::status::no_content : http::status::conflict, configured ? "" : "configuration_failed");
    }
    if (path == "/gb28181/audio/status")
    {
        session->keep_alive();
        return response(request, http::status::no_content);
    }
    session_registry::instance().remove_sender_session(id, id, *session);
    session->shutdown();
    return response(request, http::status::no_content);
}

}    // namespace media_server
