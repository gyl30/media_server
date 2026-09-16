#include <memory>
#include <utility>
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

namespace beast = boost::beast;
using tcp = boost::asio::ip::tcp;

std::string registration_body(const signaling_client_options& options)
{
    return boost::json::serialize(boost::json::object{
        {"server_id", options.server_id},
        {"instance_id", options.instance_id},
        {"control_url", options.control_url},
        {"media_ip", options.media_ip},
        {"rtmp_port", options.rtmp_port},
        {"rtsp_port", options.rtsp_port},
        {"http_port", options.http_port},
    });
}

std::string heartbeat_body(const signaling_client_options& options)
{
    return boost::json::serialize(boost::json::object{
        {"server_id", options.server_id},
        {"instance_id", options.instance_id},
    });
}

std::string publish_claim_body(const signaling_client_options& options,
                               std::string_view stream_id,
                               std::string_view protocol,
                               std::string_view stream_name)
{
    return boost::json::serialize(boost::json::object{
        {"stream_id", stream_id},
        {"server_id", options.server_id},
        {"instance_id", options.instance_id},
        {"protocol", protocol},
        {"stream_name", stream_name},
    });
}

std::string runtime_event_body(const runtime_event& event)
{
    boost::json::object body{
        {"kind", to_string(event.kind)},
        {"server_id", event.server_id},
        {"instance_id", event.instance_id},
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
    if (event.end_reason)
    {
        body.emplace("end_reason", to_string(*event.end_reason));
    }
    if (event.error)
    {
        body.emplace("error", *event.error);
    }
    return boost::json::serialize(body);
}

}    // namespace

struct signaling_client::request_state
{
    request_state(boost::asio::any_io_executor executor, std::chrono::milliseconds timeout) : resolver(executor), stream(executor), deadline(executor)
    {
        deadline.expires_after(timeout);
    }

    tcp::resolver resolver;
    beast::tcp_stream stream;
    boost::asio::steady_timer deadline;
    bool timed_out{};
};

namespace
{

void cancel_heartbeat_timer(std::shared_ptr<boost::asio::steady_timer> timer)
{
    if (!timer)
    {
        return;
    }
    const auto executor = timer->get_executor();
    boost::asio::dispatch(executor, [timer = std::move(timer)]() { static_cast<void>(timer->cancel()); });
}

}    // namespace

signaling_client& signaling_client::instance()
{
    static signaling_client value;
    return value;
}

void signaling_client::configure(boost::asio::io_context& io, signaling_client_options options)
{
    const auto parsed = boost::urls::parse_uri(options.signaling_url);
    if (!parsed || parsed->scheme() != "http" || parsed->host().empty() || parsed->has_userinfo() ||
        (!parsed->path().empty() && parsed->path() != "/") || parsed->has_query() || parsed->has_fragment())
    {
        throw std::invalid_argument("invalid signaling URL");
    }
    const std::string host{parsed->host()};
    const std::string port = parsed->has_port() ? std::string{parsed->port()} : "80";
    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
    std::shared_ptr<request_state> heartbeat_request;
    {
        std::scoped_lock lock(mutex_);
        ++generation_;
        heartbeat_timer = heartbeat_timer_.lock();
        heartbeat_timer_.reset();
        heartbeat_request = heartbeat_request_.lock();
        heartbeat_request_.reset();
        io_ = &io;
        options_ = std::move(options);
        host_ = host;
        port_ = port;
    }
    cancel_heartbeat_timer(std::move(heartbeat_timer));
    cancel_request(std::move(heartbeat_request));
}

void signaling_client::configure_mock()
{
    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
    std::shared_ptr<request_state> heartbeat_request;
    {
        std::scoped_lock lock(mutex_);
        ++generation_;
        heartbeat_timer = heartbeat_timer_.lock();
        heartbeat_timer_.reset();
        heartbeat_request = heartbeat_request_.lock();
        heartbeat_request_.reset();
        io_ = nullptr;
        options_ = {};
        host_.clear();
        port_.clear();
    }
    cancel_heartbeat_timer(std::move(heartbeat_timer));
    cancel_request(std::move(heartbeat_request));
}

signaling_request_result signaling_client::register_once(boost::asio::yield_context& yield) const
{
    std::string body;
    std::string host;
    std::string port;
    std::chrono::milliseconds timeout;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        body = registration_body(options_);
        host = host_;
        port = port_;
        timeout = options_.request_timeout;
    }
    return request("/internal/media-servers/register", std::move(body), std::move(host), std::move(port), timeout, std::nullopt, yield);
}

signaling_request_result signaling_client::heartbeat_once(boost::asio::yield_context& yield) const
{
    std::string body;
    std::string host;
    std::string port;
    std::chrono::milliseconds timeout;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        body = heartbeat_body(options_);
        host = host_;
        port = port_;
        timeout = options_.request_timeout;
    }
    return request("/internal/media-servers/heartbeat", std::move(body), std::move(host), std::move(port), timeout, std::nullopt, yield);
}

signaling_request_result signaling_client::claim_publish(std::string_view stream_id,
                                                         std::string_view protocol,
                                                         std::string_view stream_name,
                                                         boost::asio::yield_context& yield) const
{
    std::string body;
    std::string host;
    std::string port;
    std::chrono::milliseconds timeout;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        body = publish_claim_body(options_, stream_id, protocol, stream_name);
        host = host_;
        port = port_;
        timeout = options_.request_timeout;
    }
    return request("/internal/publish/claim", std::move(body), std::move(host), std::move(port), timeout, std::nullopt, yield);
}

signaling_request_result signaling_client::report_runtime_event(const runtime_event& event, boost::asio::yield_context& yield) const
{
    std::string host;
    std::string port;
    std::chrono::milliseconds timeout;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        host = host_;
        port = port_;
        timeout = options_.request_timeout;
    }
    return request("/internal/runtime-events", runtime_event_body(event), std::move(host), std::move(port), timeout, std::nullopt, yield);
}

signaling_request_result signaling_client::heartbeat_once(std::uint64_t generation, boost::asio::yield_context& yield) const
{
    std::string body;
    std::string host;
    std::string port;
    std::chrono::milliseconds timeout;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr || generation != generation_)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        body = heartbeat_body(options_);
        host = host_;
        port = port_;
        timeout = options_.request_timeout;
    }
    return request("/internal/media-servers/heartbeat", std::move(body), std::move(host), std::move(port), timeout, generation, yield);
}

signaling_request_result signaling_client::request(std::string_view target,
                                                   std::string body,
                                                   std::string host,
                                                   std::string port,
                                                   std::chrono::milliseconds timeout,
                                                   std::optional<std::uint64_t> heartbeat_generation,
                                                   boost::asio::yield_context& yield) const
{
    namespace http = beast::http;

    const auto state = std::make_shared<request_state>(yield.get_executor(), timeout);
    if (heartbeat_generation)
    {
        std::scoped_lock lock(mutex_);
        if (*heartbeat_generation != generation_)
        {
            return {.kind = signaling_result_kind::accepted, .status = 0, .error = {}};
        }
        heartbeat_request_ = state;
    }
    state->deadline.async_wait(
        [state](const boost::system::error_code& error)
        {
            if (error)
            {
                return;
            }
            state->timed_out = true;
            state->resolver.cancel();
            boost::system::error_code ignored;
            state->stream.socket().cancel(ignored);
        });

    const auto finish = [this, &state, &yield, heartbeat_generation]()
    {
        static_cast<void>(state->deadline.cancel());
        yield.get_cancellation_slot().clear();
        if (heartbeat_generation)
        {
            std::scoped_lock lock(mutex_);
            if (heartbeat_request_.lock() == state)
            {
                heartbeat_request_.reset();
            }
        }
    };
    const auto fail = [&state, &finish](const boost::system::error_code& error)
    {
        signaling_request_result result{
            .kind = signaling_result_kind::network_error,
            .error = state->timed_out ? "request timeout" : error.message(),
        };
        finish();
        return result;
    };

    boost::system::error_code error;
    const auto endpoints = state->resolver.async_resolve(host, port, yield[error]);
    if (error)
    {
        return fail(error);
    }
    static_cast<void>(state->stream.async_connect(endpoints, yield[error]));
    if (error)
    {
        return fail(error);
    }

    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, "media_server");
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    static_cast<void>(http::async_write(state->stream, request, yield[error]));
    if (error)
    {
        return fail(error);
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    static_cast<void>(http::async_read(state->stream, buffer, response, yield[error]));
    if (error)
    {
        return fail(error);
    }

    state->stream.socket().shutdown(tcp::socket::shutdown_both, error);
    const auto status = static_cast<unsigned int>(response.result_int());
    if (response.result_int() < 200 || response.result_int() >= 300)
    {
        const auto kind =
            response.result_int() >= 500 && response.result_int() < 600 ? signaling_result_kind::temporary_failure : signaling_result_kind::rejected;
        finish();
        return {.kind = kind, .status = status, .error = {}};
    }
    finish();
    return {.kind = signaling_result_kind::accepted, .status = status, .error = {}};
}

void signaling_client::cancel_request(std::shared_ptr<request_state> state)
{
    if (!state)
    {
        return;
    }
    const auto executor = state->stream.get_executor();
    boost::asio::dispatch(executor,
                          [state = std::move(state)]()
                          {
                              static_cast<void>(state->deadline.cancel());
                              state->resolver.cancel();
                              boost::system::error_code ignored;
                              state->stream.socket().cancel(ignored);
                          });
}

void signaling_client::run_heartbeat(boost::asio::yield_context& yield, std::function<void()> fenced_handler)
{
    std::shared_ptr<boost::asio::steady_timer> timer;
    std::chrono::milliseconds heartbeat_interval;
    std::uint64_t generation;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return;
        }
        heartbeat_interval = options_.heartbeat_interval;
        generation = generation_;
        timer = std::make_shared<boost::asio::steady_timer>(*io_);
        heartbeat_timer_ = timer;
    }
    const auto active = [this, generation]()
    {
        std::scoped_lock lock(mutex_);
        return generation == generation_;
    };
    const auto finish = [this, &timer]()
    {
        std::scoped_lock lock(mutex_);
        if (heartbeat_timer_.lock() == timer)
        {
            heartbeat_timer_.reset();
        }
    };
    while (yield.cancelled() == boost::asio::cancellation_type::none)
    {
        timer->expires_after(heartbeat_interval);
        boost::system::error_code error;
        timer->async_wait(yield[error]);
        if (error || !active())
        {
            finish();
            return;
        }

        const auto result = heartbeat_once(generation, yield);
        if (!active())
        {
            finish();
            return;
        }
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
            finish();
            std::scoped_lock lock(mutex_);
            if (generation != generation_)
            {
                return;
            }
            spdlog::critical("signaling heartbeat rejected status {}", result.status);
            fenced_handler();
            return;
        }
    }
    finish();
}

}    // namespace media_server
