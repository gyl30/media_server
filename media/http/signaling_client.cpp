#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <boost/url/parse.hpp>
#include <spdlog/spdlog.h>

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
    });
}

std::string heartbeat_body(const signaling_client_options& options)
{
    return boost::json::serialize(boost::json::object{
        {"server_id", options.server_id},
        {"instance_id", options.instance_id},
    });
}

struct signaling_request_state
{
    signaling_request_state(boost::asio::io_context& io, std::chrono::milliseconds timeout) : resolver(io), stream(io), deadline(io)
    {
        deadline.expires_after(timeout);
    }

    tcp::resolver resolver;
    beast::tcp_stream stream;
    boost::asio::steady_timer deadline;
    bool timed_out{};
};

}    // namespace

signaling_client::signaling_client(boost::asio::io_context& io, signaling_client_options options) : io_(io), options_(std::move(options))
{
    const auto parsed = boost::urls::parse_uri(options_.signaling_url);
    if (!parsed || parsed->scheme() != "http" || parsed->host().empty() || parsed->has_userinfo() ||
        (!parsed->path().empty() && parsed->path() != "/") || parsed->has_query() || parsed->has_fragment())
    {
        throw std::invalid_argument("invalid signaling URL");
    }
    host_ = parsed->host();
    port_ = parsed->has_port() ? parsed->port() : "80";
}

signaling_request_result signaling_client::register_once(boost::asio::yield_context& yield) const
{
    return request("/internal/media-servers/register", registration_body(options_), yield);
}

signaling_request_result signaling_client::heartbeat_once(boost::asio::yield_context& yield) const
{
    return request("/internal/media-servers/heartbeat", heartbeat_body(options_), yield);
}

signaling_request_result signaling_client::request(std::string_view target, std::string body, boost::asio::yield_context& yield) const
{
    namespace http = beast::http;

    const auto state = std::make_shared<signaling_request_state>(io_, options_.request_timeout);
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

    const auto finish = [&state]() { static_cast<void>(state->deadline.cancel()); };
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
    const auto endpoints = state->resolver.async_resolve(host_, port_, yield[error]);
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
    request.set(http::field::host, host_);
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
        const auto kind = response.result_int() >= 500 && response.result_int() < 600 ? signaling_result_kind::temporary_failure
                                                                                      : signaling_result_kind::rejected;
        finish();
        return {.kind = kind, .status = status, .error = {}};
    }

    boost::system::error_code json_error;
    const auto value = boost::json::parse(response.body(), json_error);
    if (json_error || !value.is_object())
    {
        finish();
        return {.kind = signaling_result_kind::rejected, .status = status, .error = "invalid_response"};
    }
    const auto* response_result = value.as_object().if_contains("result");
    if (response_result == nullptr || !response_result->is_string() || response_result->as_string() != "ok")
    {
        finish();
        return {.kind = signaling_result_kind::rejected, .status = status, .error = "invalid_response"};
    }

    finish();
    return {.kind = signaling_result_kind::accepted, .status = status, .error = {}};
}

void signaling_client::run_heartbeat(boost::asio::yield_context& yield, std::function<void()> fenced_handler) const
{
    boost::asio::steady_timer timer(io_);
    for (;;)
    {
        timer.expires_after(options_.heartbeat_interval);
        boost::system::error_code error;
        timer.async_wait(yield[error]);
        if (error)
        {
            return;
        }

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
            spdlog::critical("signaling heartbeat rejected status {}", result.status);
            fenced_handler();
            return;
        }
    }
}

}    // namespace media_server
