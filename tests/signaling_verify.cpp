#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <netdb.h>

#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/http/signaling_verify.h"
#include "media/http/http_target.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
std::mutex dns_mutex;
std::condition_variable dns_condition;
bool dns_entered{};
bool dns_released{};

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void http_result(boost::beast::http::status status, std::chrono::seconds delay, bool expected, bool slow_dns = false)
{
    boost::asio::io_context io;
    boost::scope::scope_exit release_dns([slow_dns]()
                                       {
                                           if (slow_dns)
                                           {
                                               std::scoped_lock lock(dns_mutex);
                                               dns_released = true;
                                               dns_condition.notify_all();
                                           }
                                       });
    boost::asio::steady_timer dns_delay(io, 1500ms);
    if (slow_dns)
    {
        {
            std::scoped_lock lock(dns_mutex);
            dns_entered = false;
            dns_released = false;
        }
        dns_delay.async_wait([](boost::system::error_code error)
                             {
                                 if (!error)
                                 {
                                     std::scoped_lock lock(dns_mutex);
                                     dns_released = true;
                                     dns_condition.notify_all();
                                 }
                             });
    }
    tcp::acceptor listener(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    config application_config;
    application_config.signaling_url = *ada::parse<ada::url_aggregator>(std::string(slow_dns ? "http://slow.verify.invalid:" : "http://127.0.0.1:") +
                                                                     std::to_string(listener.local_endpoint().port()) + "/gateway/");
    application_config.control_token = "service-test-token";
    const std::string token(64, 'a');
    const std::string stream_id = "e379e55f-a0d1-4300-a352-0a42d4a0cc09";
    std::optional<bool> result;
    std::chrono::steady_clock::duration elapsed{};
    std::exception_ptr exception;
    const auto completed = [&exception](std::exception_ptr error) { if (error) { exception = error; } };
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            tcp::socket socket(io);
            boost::system::error_code error;
            listener.async_accept(socket, yield[error]);
            require(!error, "verify server accept failed");
            boost::beast::flat_buffer buffer;
            boost::beast::http::request<boost::beast::http::string_body> request;
            boost::beast::http::async_read(socket, buffer, request, yield[error]);
            require(!error, "verify request read failed");
            require(request.method() == boost::beast::http::verb::post && request.target() == "/gateway/internal/verify", "verify endpoint changed");
            require(request[boost::beast::http::field::authorization] == "Bearer service-test-token", "service authorization missing");
            require(request[boost::beast::http::field::content_type] == "application/json", "verify content type missing");
            const auto body = boost::json::parse(request.body()).as_object();
            require(body.size() == 3 && body.at("token").as_string() == token && body.at("operation").as_string() == "play" &&
                    body.at("stream_id").as_string() == stream_id, "verify request identity changed");
            if (delay != 0s)
            {
                boost::asio::steady_timer timer(io, delay);
                timer.async_wait(yield[error]);
                require(!error, "verify server delay failed");
            }
            boost::beast::http::response<boost::beast::http::string_body> response(status, 11);
            response.body() = "verification result";
            response.prepare_payload();
            boost::beast::http::async_write(socket, response, yield[error]);
            if (delay == 0s)
            {
                require(!error, "verify response write failed");
            }
        },
        completed);
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            const auto started_at = std::chrono::steady_clock::now();
            result = verify_stream(application_config, token, "play", stream_id, yield);
            elapsed = std::chrono::steady_clock::now() - started_at;
        },
        completed);
    io.run_for(6s);
    require(io.stopped(), "verify HTTP operations exceeded timeout");
    if (exception)
    {
        std::rethrow_exception(exception);
    }
    require(result && *result == expected, "verify status decision incorrect");
    if (delay != 0s)
    {
        require(elapsed >= 2800ms && elapsed < 3500ms, "verify response timeout is not the total three-second deadline");
    }
}

void dns_deadline()
{
    boost::asio::io_context io;
    {
        std::scoped_lock lock(dns_mutex);
        dns_entered = false;
        dns_released = false;
    }
    boost::scope::scope_exit release_dns([]()
                                       {
                                           std::scoped_lock lock(dns_mutex);
                                           dns_released = true;
                                           dns_condition.notify_all();
                                       });
    tcp::acceptor listener(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    config application_config;
    application_config.signaling_url = *ada::parse<ada::url_aggregator>("http://slow.verify.invalid:" + std::to_string(listener.local_endpoint().port()));
    std::optional<bool> result;
    std::chrono::steady_clock::duration elapsed{};
    std::exception_ptr exception;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            const auto started_at = std::chrono::steady_clock::now();
            result = verify_stream(application_config, std::string(64, 'a'), "play", std::string(64, 'b'), yield);
            elapsed = std::chrono::steady_clock::now() - started_at;
        },
        [&](std::exception_ptr error) { exception = error; });
    io.run_for(3500ms);
    if (exception)
    {
        std::rethrow_exception(exception);
    }
    {
        std::scoped_lock lock(dns_mutex);
        require(dns_entered && !dns_released, "slow DNS fixture was not used");
        require(result && !*result && elapsed >= 2800ms && elapsed < 3500ms, "DNS prevented the total verification deadline");
        dns_released = true;
        dns_condition.notify_all();
    }
    io.restart();
    io.run_for(2s);
    require(io.stopped(), "late DNS completion retained pending verification");
    listener.non_blocking(true);
    tcp::socket socket(io);
    boost::system::error_code error;
    listener.accept(socket, error);
    require(error == boost::asio::error::would_block || error == boost::asio::error::try_again, "late DNS completion started a TCP connection");
}

void identifiers()
{
    require(valid_stream_token(std::string(64, 'a')), "valid token rejected");
    require(!valid_stream_token(std::string(64, 'A')) && !valid_stream_token(std::string(63, 'a')), "invalid token accepted");
    require(!valid_stream_token(std::string(61, 'a') + "%61"), "encoded token accepted");
    require(valid_stream_id("e379e55f-a0d1-4300-a352-0a42d4a0cc09") && valid_stream_id(std::string(64, 'b')), "valid stream ID rejected");
    require(!valid_stream_id("e379e55f/a0d1-4300-a352-0a42d4a0cc09") && !valid_stream_id("stream/name"), "invalid stream ID accepted");
}

void http_targets()
{
    const auto plain = parse_http_target("/play/whep/source/token");
    require(plain && plain->get_pathname() == "/play/whep/source/token" && !plain->has_search(), "HTTP origin path rejected");
    const auto query = parse_http_target("/control?value=1");
    require(query && query->get_pathname() == "/control" && query->has_search(), "HTTP query presence lost");
    const auto empty_query = parse_http_target("/control?");
    require(empty_query && empty_query->has_search(), "empty query presence lost");
    for (const auto target : {"http://localhost/play/token", "//other/play/token", "/play/token#fragment", "/play/./token", "/play/a/../token",
                              "/play/%2e/token", "/play/%2e%2e/token", "/play\\token", "/play/token\t", "/play/token "})
    {
        require(!parse_http_target(target), "normalized or non-origin HTTP target accepted");
    }
    const auto escaped = parse_http_target("/play/%61");
    require(escaped && escaped->get_pathname() == "/play/%61", "HTTP parser decoded a credential path");
}
}    // namespace

extern "C" int __real_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result);

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result)
{
    if (node != nullptr && std::strcmp(node, "slow.verify.invalid") == 0)
    {
        std::unique_lock lock(dns_mutex);
        dns_entered = true;
        dns_condition.notify_all();
        dns_condition.wait(lock, []() { return dns_released; });
        lock.unlock();
        return __real_getaddrinfo("127.0.0.1", service, hints, result);
    }
    return __real_getaddrinfo(node, service, hints, result);
}

int main()
{
    try
    {
        identifiers();
        http_targets();
        http_result(boost::beast::http::status::ok, 0s, true);
        http_result(boost::beast::http::status::forbidden, 0s, false);
        http_result(boost::beast::http::status::created, 0s, false);
        http_result(boost::beast::http::status::ok, 4s, false);
        http_result(boost::beast::http::status::ok, 2s, false, true);
        dns_deadline();
        std::cout << "signaling verify: identifiers, HTTP 200/403/201, response deadline, combined DNS/HTTP deadline and late completion passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
