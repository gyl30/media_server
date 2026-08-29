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

}    // namespace

signaling_client::signaling_client(signaling_client_options options) : options_(std::move(options))
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

signaling_client::~signaling_client() { shutdown(); }

signaling_request_result signaling_client::register_once() const
{
    return request("/internal/media-servers/register", registration_body(options_));
}

signaling_request_result signaling_client::heartbeat_once() const
{
    return request("/internal/media-servers/heartbeat", heartbeat_body(options_));
}

signaling_request_result signaling_client::request(std::string_view target, std::string body, std::stop_token stop) const
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = boost::asio::ip::tcp;

    boost::asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    boost::asio::steady_timer deadline(io, options_.request_timeout);
    signaling_request_result result;
    bool timed_out = false;
    const auto finish = [&deadline]()
    {
        static_cast<void>(deadline.cancel());
    };
    const auto fail = [&result, &timed_out, &finish](const boost::system::error_code& error)
    {
        result = {.kind = signaling_result_kind::network_error,
                  .error = timed_out ? "request timeout" : error.message()};
        finish();
    };

    deadline.async_wait(
        [&resolver, &stream, &timed_out](const boost::system::error_code& error)
        {
            if (error)
            {
                return;
            }
            timed_out = true;
            resolver.cancel();
            boost::system::error_code ignored;
            stream.socket().cancel(ignored);
        });
    std::stop_callback cancel_on_stop(
        stop,
        [&io, &resolver, &stream]()
        {
            boost::asio::post(
                io,
                [&resolver, &stream]()
                {
                    resolver.cancel();
                    boost::system::error_code ignored;
                    stream.socket().cancel(ignored);
                });
        });
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void>
        {
            boost::system::error_code error;
            const auto endpoints = co_await resolver.async_resolve(
                host_, port_, boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error)
            {
                fail(error);
                co_return;
            }
            co_await stream.async_connect(endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error)
            {
                fail(error);
                co_return;
            }

            http::request<http::string_body> request{http::verb::post, target, 11};
            request.set(http::field::host, host_);
            request.set(http::field::user_agent, "media_server");
            request.set(http::field::content_type, "application/json");
            request.body() = std::move(body);
            request.prepare_payload();
            co_await http::async_write(stream, request, boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error)
            {
                fail(error);
                co_return;
            }

            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, buffer, response, boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error)
            {
                fail(error);
                co_return;
            }
            stream.socket().shutdown(tcp::socket::shutdown_both, error);
            const auto status = static_cast<unsigned int>(response.result_int());
            if (response.result_int() < 200 || response.result_int() >= 300)
            {
                const auto kind = response.result_int() >= 500 && response.result_int() < 600
                                      ? signaling_result_kind::temporary_failure
                                      : signaling_result_kind::rejected;
                result = {.kind = kind, .status = status, .error = {}};
                finish();
                co_return;
            }

            boost::system::error_code json_error;
            const auto value = boost::json::parse(response.body(), json_error);
            if (json_error || !value.is_object())
            {
                result = {.kind = signaling_result_kind::rejected, .status = status, .error = "invalid_response"};
                finish();
                co_return;
            }
            const auto* response_result = value.as_object().if_contains("result");
            if (response_result == nullptr || !response_result->is_string() || response_result->as_string() != "ok")
            {
                result = {.kind = signaling_result_kind::rejected, .status = status, .error = "invalid_response"};
                finish();
                co_return;
            }
            result = {.kind = signaling_result_kind::accepted, .status = status, .error = {}};
            finish();
        },
        boost::asio::detached);
    io.run();
    return result;
}

void signaling_client::startup_heartbeat(std::function<void()> fenced_handler)
{
    std::lock_guard startup_lock(heartbeat_mutex_);
    if (heartbeat_thread_.joinable())
    {
        return;
    }
    heartbeat_thread_ = std::jthread(
        [this, fenced_handler = std::move(fenced_handler)](std::stop_token stop)
        {
            for (;;)
            {
                std::unique_lock lock(heartbeat_mutex_);
                heartbeat_condition_.wait_for(lock, options_.heartbeat_interval, [&stop]() { return stop.stop_requested(); });
                if (stop.stop_requested())
                {
                    return;
                }
                lock.unlock();
                const auto result = request("/internal/media-servers/heartbeat", heartbeat_body(options_), stop);
                if (stop.stop_requested())
                {
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
                    spdlog::critical("signaling heartbeat rejected status {}", result.status);
                    fenced_handler();
                    return;
                }
            }
        });
}

void signaling_client::shutdown()
{
    std::jthread heartbeat;
    {
        std::lock_guard lock(heartbeat_mutex_);
        if (!heartbeat_thread_.joinable())
        {
            return;
        }
        heartbeat_thread_.request_stop();
        heartbeat_condition_.notify_all();
        heartbeat = std::move(heartbeat_thread_);
    }
    heartbeat.join();
}

}    // namespace media_server
