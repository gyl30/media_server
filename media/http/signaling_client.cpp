#include <cstdlib>
#include <utility>
#include <iterator>
#include <stdexcept>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>
#include <boost/url/parse.hpp>

#include "media/core/runtime_event.h"
#include "media/http/signaling_client.h"

namespace media_server
{

namespace
{

std::string registration_body(const config& cfg, std::string_view instance_id)
{
    return boost::json::serialize(boost::json::object{
        {"server_id", cfg.server_id},
        {"instance_id", instance_id},
        {"control_url", cfg.control_url},
        {"media_ip", cfg.media_ip},
        {"rtmp_port", cfg.rtmp_port},
        {"rtsp_port", cfg.rtsp_port},
        {"http_port", cfg.http_port},
    });
}

std::string heartbeat_body(const config& cfg, std::string_view instance_id)
{
    return boost::json::serialize(boost::json::object{
        {"server_id", cfg.server_id},
        {"instance_id", instance_id},
    });
}

std::string publish_claim_body(const config& cfg,
                               std::string_view instance_id,
                               std::string_view stream_id,
                               std::string_view protocol,
                               std::string_view stream_name)
{
    return boost::json::serialize(boost::json::object{
        {"stream_id", stream_id},
        {"server_id", cfg.server_id},
        {"instance_id", instance_id},
        {"protocol", protocol},
        {"stream_name", stream_name},
    });
}

boost::json::object runtime_event_json(const runtime_event& event)
{
    boost::json::object body{
        {"kind", to_string(event.kind)},
        {"stream_id", event.stream_id},
        {"stream_name", event.stream_name},
        {"protocol", to_string(event.protocol)},
        {"state", to_string(event.state)},
    };
    if (event.source_id)
    {
        body.emplace("source_id", *event.source_id);
    }
    if (event.stage)
    {
        body.emplace("stage", *event.stage);
    }
    if (event.error)
    {
        body.emplace("error", *event.error);
    }
    return body;
}

std::string runtime_event_batch_body(const config& cfg, std::string_view instance_id, const std::vector<runtime_event>& batch)
{
    boost::json::array events;
    events.reserve(batch.size());
    for (const auto& event : batch)
    {
        events.push_back(runtime_event_json(event));
    }
    return boost::json::serialize(boost::json::object{
        {"server_id", cfg.server_id},
        {"instance_id", instance_id},
        {"events", std::move(events)},
    });
}

}    // namespace

signaling_client& signaling_client::instance()
{
    static signaling_client value;
    return value;
}

void signaling_client::configure(const config& cfg,
                                 std::string instance_id,
                                 std::chrono::milliseconds heartbeat_interval,
                                 std::chrono::milliseconds request_timeout)
{
    if (cfg.signaling_url.empty())
    {
        config_ = cfg;
        instance_id_ = std::move(instance_id);
        heartbeat_interval_ = heartbeat_interval;
        request_timeout_ = request_timeout;
        host_.clear();
        port_.clear();
        return;
    }

    const auto parsed = boost::urls::parse_uri(cfg.signaling_url);
    if (!parsed || parsed->scheme() != "http" || parsed->host().empty() || parsed->has_userinfo() ||
        (!parsed->path().empty() && parsed->path() != "/") || parsed->has_query() || parsed->has_fragment())
    {
        throw std::invalid_argument("invalid signaling URL");
    }

    config_ = cfg;
    instance_id_ = std::move(instance_id);
    heartbeat_interval_ = heartbeat_interval;
    request_timeout_ = request_timeout;
    host_ = std::string{parsed->host()};
    port_ = parsed->has_port() ? std::string{parsed->port()} : "80";
}

signaling_request_result signaling_client::register_once(boost::asio::yield_context& yield) const
{
    if (host_.empty())
    {
        return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
    }
    return request("/internal/media-servers/register", registration_body(config_, instance_id_), host_, port_, request_timeout_, yield);
}

signaling_request_result signaling_client::heartbeat_once(boost::asio::yield_context& yield) const
{
    if (host_.empty())
    {
        return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
    }
    return request("/internal/media-servers/heartbeat", heartbeat_body(config_, instance_id_), host_, port_, request_timeout_, yield);
}

signaling_request_result signaling_client::claim_publish(std::string_view stream_id,
                                                         std::string_view protocol,
                                                         std::string_view stream_name,
                                                         boost::asio::yield_context& yield) const
{
    if (host_.empty())
    {
        return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
    }
    return request(
        "/internal/publish/claim", publish_claim_body(config_, instance_id_, stream_id, protocol, stream_name), host_, port_, request_timeout_, yield);
}

void signaling_client::report(runtime_event event)
{
    if (host_.empty())
    {
        return;
    }
    bool overflow{};
    std::size_t pending{};
    const auto newest_stream_id = event.stream_id;
    {
        std::scoped_lock lock(event_mutex_);
        if (pending_events_.size() >= max_pending_events)
        {
            overflow = true;
            pending = pending_events_.size();
            pending_events_.clear();
        }
        pending_events_.push_back(std::move(event));
    }
    if (overflow)
    {
        spdlog::warn("runtime event queue full pending {} limit {} newest stream_id {}", pending, max_pending_events, newest_stream_id);
    }
}

signaling_request_result signaling_client::request(std::string_view target,
                                                   std::string body,
                                                   std::string host,
                                                   std::string port,
                                                   std::chrono::milliseconds timeout,
                                                   boost::asio::yield_context& yield) const
{
    boost::asio::ip::tcp::resolver resolver(yield.get_executor());
    boost::beast::tcp_stream stream(yield.get_executor());
    boost::system::error_code error;
    const auto endpoints = resolver.async_resolve(host, port, yield[error]);
    if (error)
    {
        return {.kind = signaling_result_kind::network_error, .error = error.message()};
    }
    stream.expires_after(timeout);
    static_cast<void>(stream.async_connect(endpoints, yield[error]));
    if (error)
    {
        return {.kind = signaling_result_kind::network_error,
                .error = error == boost::beast::error::timeout ? "request timeout" : error.message()};
    }

    boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::post, target, 11};
    request.set(boost::beast::http::field::host, host);
    request.set(boost::beast::http::field::user_agent, "media_server");
    request.set(boost::beast::http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    static_cast<void>(boost::beast::http::async_write(stream, request, yield[error]));
    if (error)
    {
        return {.kind = signaling_result_kind::network_error,
                .error = error == boost::beast::error::timeout ? "request timeout" : error.message()};
    }

    boost::beast::flat_buffer buffer;
    boost::beast::http::response<boost::beast::http::string_body> response;
    static_cast<void>(boost::beast::http::async_read(stream, buffer, response, yield[error]));
    if (error)
    {
        return {.kind = signaling_result_kind::network_error,
                .error = error == boost::beast::error::timeout ? "request timeout" : error.message()};
    }

    stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    const auto status = static_cast<unsigned int>(response.result_int());
    if (response.result_int() < 200 || response.result_int() >= 300)
    {
        const auto kind =
            response.result_int() >= 500 && response.result_int() < 600 ? signaling_result_kind::temporary_failure : signaling_result_kind::rejected;
        return {.kind = kind, .status = status, .error = {}};
    }
    return {.kind = signaling_result_kind::accepted, .status = status, .error = {}};
}

void signaling_client::run(boost::asio::yield_context& yield)
{
    if (host_.empty())
    {
        return;
    }
    boost::asio::steady_timer timer(yield.get_executor());
    std::vector<runtime_event> events;
    for (;;)
    {
        timer.expires_after(heartbeat_interval_);
        timer.async_wait(yield);

        const auto result = heartbeat_once(yield);
        if (result.kind == signaling_result_kind::network_error)
        {
            spdlog::warn("signaling heartbeat network error {}", result.error);
            continue;
        }
        if (result.kind == signaling_result_kind::temporary_failure)
        {
            spdlog::warn("signaling heartbeat temporary failure status {}", result.status);
            continue;
        }
        if (result.kind == signaling_result_kind::rejected)
        {
            spdlog::critical("signaling heartbeat rejected status {}; aborting in 5 seconds", result.status);
            boost::asio::steady_timer abort_timer(yield.get_executor(), std::chrono::seconds{5});
            abort_timer.async_wait(yield);
            std::abort();
        }

        {
            std::scoped_lock lock(event_mutex_);
            if (events.size() + pending_events_.size() > max_pending_events)
            {
                events.clear();
            }
            events.insert(events.end(), std::make_move_iterator(pending_events_.begin()), std::make_move_iterator(pending_events_.end()));
            pending_events_.clear();
        }
        if (events.empty())
        {
            continue;
        }
        const auto event_result =
            request("/internal/runtime-events", runtime_event_batch_body(config_, instance_id_, events), host_, port_, request_timeout_, yield);
        if (event_result.kind == signaling_result_kind::accepted)
        {
            events.clear();
            continue;
        }
        if (event_result.kind == signaling_result_kind::network_error)
        {
            spdlog::warn("runtime event batch network error {}; retaining {} events", event_result.error, events.size());
        }
        else
        {
            spdlog::warn("runtime event batch delivery failed status {}; retaining {} events", event_result.status, events.size());
        }
    }
}

}    // namespace media_server
