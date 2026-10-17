#include <array>
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <functional>
#include <string_view>

#include <boost/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/scope/scope_exit.hpp>

#include "config.h"
#include "media/http/http_session.h"
#include "media/net/worker_context.h"
#include "media/net/media_port_pool.h"
#include "media/core/session_registry.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;
using error_code = boost::system::error_code;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using ws_stream = websocket::stream<tcp::socket>;
constexpr auto limit = 3s;
constexpr std::uint16_t first_port = 24'620;
constexpr std::uint32_t ssrc = 0x12345678;
const auto loopback = boost::asio::ip::address_v4::loopback();
const std::string identity(64, 'a');
const std::string token(64, 'b');
const std::string wrong_token(64, 'c');
const std::string control_secret = "audio-upload-controlled-test";
const std::string base = "/gb28181/audio/";

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void on(worker_context& worker, Function function)
{
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    boost::asio::post(worker.io(),
                      [promise, function = std::move(function)]() mutable
                      {
                          try
                          {
                              function();
                              promise->set_value();
                          }
                          catch (...)
                          {
                              promise->set_exception(std::current_exception());
                          }
                      });
    require(future.wait_for(limit) == std::future_status::ready, "owner barrier timeout");
    future.get();
}

template <typename Socket, typename Begin>
error_code operation(boost::asio::io_context& io, Socket& socket, Begin begin, std::chrono::milliseconds timeout = limit)
{
    io.restart();
    boost::asio::steady_timer timer(io, timeout);
    bool finished{};
    bool expired{};
    error_code result;
    timer.async_wait(
        [&](error_code error)
        {
            if (!error)
            {
                expired = true;
                error_code ignored;
                socket.close(ignored);
            }
        });
    begin(
        [&](error_code error, std::size_t)
        {
            result = error;
            finished = true;
            timer.cancel();
        });
    io.run();
    require(finished && !expired, "peer operation timeout");
    return result;
}

struct fixture
{
    worker_context owner;
    worker_context ingress;
    boost::asio::io_context io;
    config settings;
    udp::socket rtp{io, {loopback, 0}};
    udp::socket rtcp{io, {loopback, 0}};
    std::vector<std::weak_ptr<session>> lifetimes;
    std::vector<std::weak_ptr<http_session>> http_lifetimes;
    std::jthread owner_thread;
    std::jthread ingress_thread;
    bool joined{};

    fixture()
    {
        settings.control_token = control_secret;
        owner_thread = std::jthread([this]() { owner.run(); });
        ingress_thread = std::jthread([this]() { ingress.run(); });
    }

    ~fixture()
    {
        if (!joined)
        {
            if (const auto current = session_registry::instance().find_sender_session(identity, identity))
            {
                current->shutdown();
            }
            owner.request_stop();
            ingress.request_stop();
            owner_thread.join();
            ingress_thread.join();
        }
    }

    tcp::socket connect(worker_context& worker)
    {
        auto listener = std::make_shared<tcp::acceptor>(worker.io(), tcp::endpoint{loopback, 0});
        boost::scope::scope_exit close_listener(
            [listener, executor = worker.io().get_executor()]()
            {
                boost::asio::post(executor,
                                  [listener]()
                                  {
                                      error_code ignored;
                                      listener->close(ignored);
                                  });
            });
        auto accepted = std::make_shared<tcp::socket>(worker.io());
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        on(worker,
           [this, &worker, listener, accepted, promise]()
           {
               listener->async_accept(*accepted,
                                      [this, &worker, listener, accepted, promise](error_code error)
                                      {
                                          if (error)
                                          {
                                              promise->set_exception(std::make_exception_ptr(std::runtime_error("HTTP accept failed")));
                                              return;
                                          }
                                          auto connection = std::make_shared<http_session>(worker, std::move(*accepted), settings);
                                          http_lifetimes.push_back(connection);
                                          connection->startup();
                                          promise->set_value();
                                      });
           });
        tcp::socket socket(io);
        require(!operation(
                    io, socket, [&](auto done) { socket.async_connect(listener->local_endpoint(), [done](error_code error) { done(error, 0); }); }),
                "HTTP connect failed");
        require(future.wait_for(limit) == std::future_status::ready, "HTTP accept timeout");
        future.get();
        return socket;
    }

    http::response<http::string_body> control(std::string_view action, boost::json::object body, http::status expected, bool authorized = true)
    {
        // 创建固定在媒体 owner；其余控制从不同 worker 进入，覆盖 completion 返回原调用方。
        auto socket = connect(action == "create" ? owner : ingress);
        http::request<http::string_body> request(http::verb::post, base + std::string(action), 11);
        request.set(http::field::host, "localhost");
        request.set(http::field::content_type, "application/json");
        if (authorized)
        {
            request.set(http::field::authorization, "Bearer " + control_secret);
        }
        request.body() = boost::json::serialize(body);
        request.prepare_payload();
        require(!operation(io, socket, [&](auto done) { http::async_write(socket, request, done); }), "control request failed");
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> response;
        require(!operation(io, socket, [&](auto done) { http::async_read(socket, buffer, response, done); }), "control response failed");
        require(expected == http::status::unknown || response.result() == expected, "unexpected control status");
        return response;
    }

    std::uint16_t create()
    {
        const auto response = control("create", {{"session_id", identity}}, http::status::created);
        const auto value = boost::json::parse(response.body()).as_object().at("rtp_port").as_int64();
        require(value >= first_port && value <= first_port + 6 && value % 2 == 0, "create returned invalid port");
        const auto current = session_registry::instance().find_sender_session(identity, identity);
        require(current != nullptr, "create omitted registry ownership");
        lifetimes.push_back(current);
        return static_cast<std::uint16_t>(value);
    }

    void configure(const std::string& credential = token, http::status expected = http::status::no_content)
    {
        control("configure",
                {{"session_id", identity},
                 {"upload_token", credential},
                 {"remote_address", "127.0.0.1"},
                 {"rtp_port", rtp.local_endpoint().port()},
                 {"rtcp_port", rtcp.local_endpoint().port()},
                 {"ssrc", ssrc}},
                expected);
    }

    std::unique_ptr<ws_stream> upgrade(const std::string& credential = token,
                                       http::status expected = http::status::switching_protocols,
                                       bool include_token = true)
    {
        auto stream = std::make_unique<ws_stream>(connect(ingress));
        http::response<http::string_body> response;
        const auto target = "/publish/gb28181/audio/" + identity + (include_token ? "/" + credential : "");
        const auto error =
            operation(io,
                      stream->next_layer(),
                      [&](auto done) { stream->async_handshake(response, "localhost", target, [done](error_code result) { done(result, 0); }); });
        require(response.result() == expected, "unexpected websocket upgrade status");
        require((expected == http::status::switching_protocols) == !error, "upgrade error/status mismatch");
        return stream;
    }

    void send(ws_stream& stream, const std::vector<std::uint8_t>& bytes, bool binary = true)
    {
        stream.binary(binary);
        require(!operation(io, stream.next_layer(), [&](auto done) { stream.async_write(boost::asio::buffer(bytes), done); }),
                "websocket audio write failed");
    }

    std::vector<std::uint8_t> receive(std::uint16_t port)
    {
        std::array<std::uint8_t, 2048> buffer{};
        udp::endpoint peer;
        std::size_t count{};
        require(!operation(io,
                           rtp,
                           [&](auto done)
                           {
                               rtp.async_receive_from(boost::asio::buffer(buffer),
                                                      peer,
                                                      [&, done](error_code error, std::size_t bytes)
                                                      {
                                                          count = bytes;
                                                          done(error, bytes);
                                                      });
                           }),
                "RTP receive failed");
        require(peer == udp::endpoint(loopback, port), "RTP bound source mismatch");
        return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    void closed(ws_stream& stream, std::chrono::milliseconds timeout = limit)
    {
        boost::beast::flat_buffer buffer;
        require(static_cast<bool>(operation(
                    io, stream.next_layer(), [&](auto done) { stream.async_read(buffer, done); }, timeout)),
                "closed upload still accepted websocket media");
    }

    void wait_unregistered()
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto status = control("status", {{"session_id", identity}}, http::status::unknown).result();
            if (status == http::status::not_found)
            {
                return;
            }
            require(status == http::status::no_content, "disconnect cleanup status failed");
            io.restart();
            boost::asio::steady_timer retry(io, 10ms);
            retry.async_wait([](error_code) {});
            io.run();
        }
        throw std::runtime_error("websocket disconnect did not unregister without explicit shutdown");
    }

    void finish()
    {
        if (const auto current = session_registry::instance().find_sender_session(identity, identity))
        {
            current->shutdown();
            current->shutdown();
        }
        owner.request_stop();
        ingress.request_stop();
        owner_thread.join();
        ingress_thread.join();
        joined = true;
        require(!session_registry::instance().find_sender_session(identity, identity), "audio session remains registered after stop");
        for (const auto& lifetime : lifetimes)
        {
            require(lifetime.expired(), "audio session callback reference cycle");
        }
        for (const auto& lifetime : http_lifetimes)
        {
            require(lifetime.expired(), "HTTP request retained after stop");
        }
        std::vector<std::uint16_t> ports;
        while (const auto port = media_port_pool::instance().acquire())
        {
            ports.push_back(*port);
            udp::socket rtp_probe(io, {loopback, *port});
            udp::socket rtcp_probe(io, {loopback, static_cast<std::uint16_t>(*port + 1U)});
        }
        for (const auto port : ports)
        {
            media_port_pool::instance().release(port);
        }
        require(ports.size() == 4, "audio media port leak or duplicate return");
    }
};

std::uint32_t u32(const std::vector<std::uint8_t>& packet, std::size_t offset)
{
    return (static_cast<std::uint32_t>(packet.at(offset)) << 24U) | (static_cast<std::uint32_t>(packet.at(offset + 1U)) << 16U) |
           (static_cast<std::uint32_t>(packet.at(offset + 2U)) << 8U) | packet.at(offset + 3U);
}

std::uint16_t u16(const std::vector<std::uint8_t>& packet, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(packet.at(offset)) << 8U) | packet.at(offset + 1U));
}

void control_and_credentials()
{
    fixture f;
    f.control("create", {{"session_id", identity}}, http::status::forbidden, false);
    f.control("configure", {{"session_id", identity}}, http::status::forbidden, false);
    f.control("status", {{"session_id", identity}}, http::status::forbidden, false);
    f.control("delete", {{"session_id", identity}}, http::status::forbidden, false);
    f.control("status", {{"session_id", identity}}, http::status::not_found);
    f.control("delete", {{"session_id", identity}}, http::status::not_found);
    const auto port = f.create();
    f.control("create", {{"session_id", identity}}, http::status::conflict);
    f.upgrade(token, http::status::forbidden);
    f.configure();
    f.configure(wrong_token, http::status::conflict);
    f.upgrade(token, http::status::bad_request, false);
    f.upgrade(wrong_token, http::status::forbidden);
    auto stream = f.upgrade();
    f.upgrade(token, http::status::forbidden);
    f.upgrade(wrong_token, http::status::forbidden);
    std::vector<std::uint8_t> samples(160, 0xd5);
    f.send(*stream, samples);
    const auto first = f.receive(port);
    std::fill(samples.begin(), samples.end(), 0x55);
    f.send(*stream, samples);
    const auto second = f.receive(port);
    require(first.size() == 172 && second.size() == 172 && first[0] == 0x80 && (first[1] & 0x7fU) == 8, "PCMA RTP framing mismatch");
    require(u32(first, 8) == ssrc && u32(second, 8) == ssrc, "RTP SSRC changed");
    require(static_cast<std::uint16_t>(u16(first, 2) + 1U) == u16(second, 2) && u32(second, 4) - u32(first, 4) == 160,
            "RTP sequence/timestamp progression mismatch");
    require(std::all_of(first.begin() + 12, first.end(), [](auto value) { return value == 0xd5; }) &&
                std::all_of(second.begin() + 12, second.end(), [](auto value) { return value == 0x55; }),
            "websocket-to-RTP sample corruption");
    f.control("status", {{"session_id", identity}}, http::status::no_content);
    error_code ignored;
    stream->next_layer().close(ignored);
    f.wait_unregistered();
    f.finish();
}

void concurrent_claim()
{
    fixture f;
    f.create();
    f.configure();
    ws_stream first(f.connect(f.ingress));
    ws_stream second(f.connect(f.ingress));
    std::array<http::response<http::string_body>, 2> responses;
    std::array<error_code, 2> errors;
    unsigned completed{};
    boost::asio::steady_timer deadline(f.io, limit);
    bool expired{};
    deadline.async_wait(
        [&](error_code error)
        {
            if (!error)
            {
                expired = true;
                error_code ignored;
                first.next_layer().close(ignored);
                second.next_layer().close(ignored);
            }
        });
    const auto path = "/publish/gb28181/audio/" + identity + "/" + token;
    for (unsigned index = 0; index < 2; ++index)
    {
        auto& stream = index == 0 ? first : second;
        stream.async_handshake(responses[index],
                               "localhost",
                               path,
                               [&, index](error_code error)
                               {
                                   errors[index] = error;
                                   if (++completed == 2)
                                   {
                                       deadline.cancel();
                                   }
                               });
    }
    f.io.restart();
    f.io.run();
    require(!expired && completed == 2, "concurrent claims timed out");
    const bool first_won = responses[0].result() == http::status::switching_protocols;
    require(responses[first_won ? 1 : 0].result() == http::status::forbidden && !errors[first_won ? 0 : 1] &&
                static_cast<bool>(errors[first_won ? 1 : 0]),
            "same upload token admitted more or less than one viewer");
    error_code ignored;
    first.next_layer().close(ignored);
    second.next_layer().close(ignored);
    f.finish();
}

void invalid_frames()
{
    for (const auto size : {0U, 159U, 161U, 160U})
    {
        fixture f;
        f.create();
        f.configure();
        auto stream = f.upgrade();
        f.send(*stream, std::vector<std::uint8_t>(size, 0xd5), size != 160U);
        f.closed(*stream);
        f.rtp.non_blocking(true);
        std::array<std::uint8_t, 2048> bytes{};
        udp::endpoint peer;
        error_code error;
        f.rtp.receive_from(boost::asio::buffer(bytes), peer, 0, error);
        require(error == boost::asio::error::would_block || error == boost::asio::error::try_again, "invalid websocket message emitted RTP");
        f.finish();
    }
}

void delete_and_worker_stop()
{
    for (const bool explicit_delete : {true, false})
    {
        fixture f;
        f.create();
        f.configure();
        auto stream = f.upgrade();
        f.send(*stream, std::vector<std::uint8_t>(160, 0xd5));
        if (explicit_delete)
        {
            auto closing = session_registry::instance().find_sender_session(identity, identity);
            f.control("delete", {{"session_id", identity}}, http::status::no_content);
            f.closed(*stream);
            f.control("status", {{"session_id", identity}}, http::status::not_found);
            f.control("delete", {{"session_id", identity}}, http::status::not_found);
            closing->shutdown();
            closing->shutdown();
            closing.reset();
        }
        // No delay before worker stop: UDP completion, cross-worker WS callbacks and repeated shutdown may all be pending.
        f.finish();
    }
}

void control_lease_with_active_media()
{
    fixture f;
    f.create();
    f.configure();
    auto stream = f.upgrade();
    stream->binary(true);
    auto control = f.connect(f.ingress);
    http::request<http::string_body> renewal_request(http::verb::post, base + "status", 11);
    renewal_request.set(http::field::host, "localhost");
    renewal_request.set(http::field::content_type, "application/json");
    renewal_request.set(http::field::authorization, "Bearer " + control_secret);
    renewal_request.body() = boost::json::serialize(boost::json::object{{"session_id", identity}});
    renewal_request.prepare_payload();
    http::response<http::string_body> renewal_response;
    boost::beast::flat_buffer renewal_buffer;
    boost::asio::steady_timer renewal(f.io, 10s);
    boost::asio::steady_timer renewal_deadline(f.io);
    bool renewed{};
    renewal.async_wait(
        [&](error_code error)
        {
            if (error)
            {
                return;
            }
            renewal_deadline.expires_after(limit);
            renewal_deadline.async_wait(
                [&](error_code timeout_error)
                {
                    if (!timeout_error)
                    {
                        error_code ignored;
                        control.close(ignored);
                    }
                });
            http::async_write(control,
                              renewal_request,
                              [&](error_code write_error, std::size_t)
                              {
                                  if (write_error)
                                  {
                                      renewal_deadline.cancel();
                                      return;
                                  }
                                  http::async_read(control,
                                                   renewal_buffer,
                                                   renewal_response,
                                                   [&](error_code read_error, std::size_t)
                                                   {
                                                       renewed = !read_error && renewal_response.result() == http::status::no_content;
                                                       renewal_deadline.cancel();
                                                   });
                              });
        });
    std::vector<std::uint8_t> samples(160, 0xd5);
    boost::asio::steady_timer pace(f.io);
    boost::beast::flat_buffer buffer;
    unsigned sent{};
    unsigned received{};
    bool valid_rtp = true;
    auto last_rtp = std::chrono::steady_clock::time_point{};
    bool closed{};
    std::array<std::uint8_t, 2048> datagram{};
    udp::endpoint remote;
    std::function<void()> receive;
    receive = [&]()
    {
        f.rtp.async_receive_from(boost::asio::buffer(datagram),
                                 remote,
                                 [&](error_code error, std::size_t bytes)
                                 {
                                     if (error)
                                     {
                                         valid_rtp = valid_rtp && error == boost::asio::error::operation_aborted;
                                         return;
                                     }
                                     const std::vector<std::uint8_t> packet(datagram.begin(), datagram.begin() + static_cast<std::ptrdiff_t>(bytes));
                                     valid_rtp =
                                         valid_rtp && packet.size() == 172 && packet[0] == 0x80 && (packet[1] & 0x7fU) == 8 && u32(packet, 8) == ssrc;
                                     ++received;
                                     last_rtp = std::chrono::steady_clock::now();
                                     if (!closed)
                                     {
                                         receive();
                                     }
                                 });
    };
    std::function<void()> write;
    write = [&]()
    {
        stream->async_write(boost::asio::buffer(samples),
                            [&](error_code error, std::size_t)
                            {
                                if (error || closed)
                                {
                                    return;
                                }
                                ++sent;
                                pace.expires_after(250ms);
                                pace.async_wait(
                                    [&](error_code timer_error)
                                    {
                                        if (!timer_error && !closed)
                                        {
                                            write();
                                        }
                                    });
                            });
    };
    const auto begin = std::chrono::steady_clock::now();
    receive();
    write();
    const auto error = operation(
        f.io,
        stream->next_layer(),
        [&](auto done)
        {
            stream->async_read(buffer,
                               [&, done](error_code result, std::size_t bytes)
                               {
                                   closed = true;
                                   pace.cancel();
                                   error_code ignored;
                                   f.rtp.cancel(ignored);
                                   done(result, bytes);
                               });
        },
        33s);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    require(renewed && error && sent >= 100 && elapsed >= 29s, "active media did not end at renewed control lease timeout");
    require(valid_rtp && received >= 100 && last_rtp - begin >= 28s, "control lease test lacked continuing real PCMA RTP progress");
    f.control("status", {{"session_id", identity}}, http::status::not_found);
    f.finish();
}

void audio_idle_with_active_control()
{
    fixture f;
    f.create();
    f.configure();
    auto stream = f.upgrade();
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + 23s;
    unsigned renewals{};
    bool ended{};
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto result = f.control("status", {{"session_id", identity}}, http::status::unknown).result();
        if (result == http::status::not_found)
        {
            ended = true;
            break;
        }
        require(result == http::status::no_content, "audio-idle control renewal failed");
        ++renewals;
        // 等待下一次实际租约续期，而不是用 sleep 等待清理碰巧完成。
        f.io.restart();
        boost::asio::steady_timer renewal(f.io, 1s);
        renewal.async_wait([](error_code) {});
        f.io.run();
    }
    require(ended && renewals >= 15 && std::chrono::steady_clock::now() - begin >= 19s, "control renewals masked audio-idle timeout");
    f.closed(*stream);
    f.finish();
}
}    // namespace

int main()
{
    media_port_pool::init(first_port, first_port + 7);
    const std::array<std::pair<const char*, void (*)()>, 6> cases{{
        {"control_credentials_and_cross_worker_audio", control_and_credentials},
        {"concurrent_token_claim", concurrent_claim},
        {"invalid_websocket_frames", invalid_frames},
        {"delete_worker_stop_and_late_callbacks", delete_and_worker_stop},
        {"real_control_lease_with_active_media", control_lease_with_active_media},
        {"real_audio_idle_with_active_control", audio_idle_with_active_control},
    }};
    for (const auto& [name, function] : cases)
    {
        try
        {
            function();
            std::cout << "[pass] " << name << '\n';
        }
        catch (const std::exception& error)
        {
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
}
