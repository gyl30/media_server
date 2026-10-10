#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/async_result.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "media/http/signaling_verify.h"

namespace media_server
{

bool valid_stream_token(std::string_view token) noexcept
{
    return token.size() == 64 && std::ranges::all_of(token, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

bool valid_stream_id(std::string_view stream_id) noexcept
{
    if (valid_stream_token(stream_id))
    {
        return true;
    }
    if (stream_id.size() != 36)
    {
        return false;
    }
    for (std::size_t index = 0; index < stream_id.size(); ++index)
    {
        const auto value = stream_id[index];
        if (index == 8 || index == 13 || index == 18 || index == 23)
        {
            if (value != '-')
            {
                return false;
            }
        }
        else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
        {
            return false;
        }
    }
    return true;
}

bool verify_stream(const config& application_config,
                   std::string_view token,
                   std::string_view operation,
                   std::string_view stream_id,
                   boost::asio::yield_context yield)
{
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + std::chrono::seconds(3);
    std::string_view stage = "resolve";
    try
    {
        const auto& url = application_config.signaling_url;
        auto hostname = url.get_hostname();
        if (hostname.starts_with('[') && hostname.ends_with(']'))
        {
            hostname.remove_prefix(1);
            hostname.remove_suffix(1);
        }
        const std::string host{hostname};
        const std::string port = url.get_port().empty() ? "80" : std::string(url.get_port());
        spdlog::debug("signaling verify url {} token {} operation {} stream_id {}", url.get_href(), token, operation, stream_id);

        boost::system::error_code error;
        auto resolve_token = yield[error];
        const auto endpoints = boost::asio::async_initiate<decltype(resolve_token), void(boost::system::error_code, boost::asio::ip::tcp::resolver::results_type)>(
            [&host, &port, deadline, executor = yield.get_executor()](auto handler)
            {
                auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(executor);
                auto timer = std::make_shared<boost::asio::steady_timer>(executor, deadline);
                auto completion = std::make_shared<std::optional<decltype(handler)>>(std::move(handler));
                resolver->async_resolve(
                    host,
                    port,
                    [resolver, timer, completion](boost::system::error_code resolve_error, boost::asio::ip::tcp::resolver::results_type result) mutable
                    {
                        if (!*completion)
                        {
                            return;
                        }
                        timer->cancel();
                        auto callback = std::move(**completion);
                        completion->reset();
                        callback(resolve_error, std::move(result));
                    });
                timer->async_wait(
                    [resolver, timer, completion](boost::system::error_code timer_error) mutable
                    {
                        if (timer_error || !*completion)
                        {
                            return;
                        }
                        resolver->cancel();
                        auto callback = std::move(**completion);
                        completion->reset();
                        callback(boost::asio::error::timed_out, {});
                    });
            },
            resolve_token);
        if (!error)
        {
            boost::beast::tcp_stream connection(yield.get_executor());
            connection.expires_at(deadline);
            stage = "connect";
            connection.async_connect(endpoints, yield[error]);
            if (!error)
            {
                std::string target{url.get_pathname()};
                if (!target.empty() && target.back() == '/')
                {
                    target.pop_back();
                }
                target.append("/internal/verify");
                boost::beast::http::request<boost::beast::http::string_body> request(boost::beast::http::verb::post, target, 11);
                request.set(boost::beast::http::field::host, url.get_host());
                request.set(boost::beast::http::field::content_type, "application/json");
                if (!application_config.control_token.empty())
                {
                    request.set(boost::beast::http::field::authorization, "Bearer " + application_config.control_token);
                }
                request.keep_alive(false);
                request.body() = boost::json::serialize(boost::json::object{{"token", token}, {"operation", operation}, {"stream_id", stream_id}});
                request.prepare_payload();
                stage = "write";
                boost::beast::http::async_write(connection, request, yield[error]);
                if (!error)
                {
                    stage = "read";
                    boost::beast::flat_buffer buffer;
                    boost::beast::http::response_parser<boost::beast::http::string_body> response;
                    response.body_limit(64U * 1024U);
                    boost::beast::http::async_read(connection, buffer, response, yield[error]);
                    if (!error)
                    {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count();
                        spdlog::debug("signaling verify result token {} operation {} stream_id {} status {} body {} elapsed_ms {}",
                                      token, operation, stream_id, response.get().result_int(), response.get().body(), elapsed);
                        return response.get().result() == boost::beast::http::status::ok;
                    }
                }
            }
        }
        spdlog::debug("signaling verify failed token {} operation {} stream_id {} stage {} error {} elapsed_ms {}",
                      token, operation, stream_id, stage, error.message(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
    }
    catch (const std::exception& error)
    {
        spdlog::error("signaling verify failed token {} operation {} stream_id {} stage {} exception {}", token, operation, stream_id, stage, error.what());
    }
    return false;
}

}    // namespace media_server
