#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <ada.h>
#include <boost/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/scope/scope_exit.hpp>

#include "config.h"
#include "media/http/http_session.h"
#include "media/net/worker_context.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_playback_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
using error_code = boost::system::error_code;
namespace http = boost::beast::http;
constexpr auto limit = 2s;
const std::string stream_id(64, 'a');
const std::string token(64, 'b');
const std::string path = "/play/gb28181/" + stream_id + "/" + token + ".ps";

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T>
T ready(std::future<T>& result)
{
    require(result.wait_for(limit) == std::future_status::ready, "worker barrier deadline");
    return result.get();
}

template <typename Function>
void on(worker_context& worker, Function function)
{
    auto result = std::make_shared<std::promise<void>>();
    auto future = result->get_future();
    boost::asio::post(worker.io(),
                      [result, function = std::move(function)]() mutable
                      {
                          try
                          {
                              function();
                              result->set_value();
                          }
                          catch (...)
                          {
                              result->set_exception(std::current_exception());
                          }
                      });
    ready(future);
}

template <typename Begin>
error_code peer_operation(boost::asio::io_context& io, tcp::socket& socket, Begin begin)
{
    io.restart();
    boost::asio::steady_timer timer(io, limit);
    bool complete = false;
    bool expired = false;
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
            complete = true;
            timer.cancel();
        });
    io.run();
    require(complete && !expired, "peer IO deadline");
    return result;
}

std::string request(std::string_view target, std::string_view method = "GET", std::string_view version = "HTTP/1.1", std::string_view body = {})
{
    std::string result = std::string(method) + " " + std::string(target) + " " + std::string(version) + "\r\nHost: localhost\r\n";
    if (!body.empty())
    {
        result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    return result + "\r\n" + std::string(body);
}

class other_receiver final : public session
{
   public:
    void shutdown() override {}
};

class fixture
{
   public:
    fixture() : verify_listener(worker.io(), {boost::asio::ip::address_v4::loopback(), 0})
    {
        application_config.signaling_url =
            *ada::parse<ada::url_aggregator>("http://127.0.0.1:" + std::to_string(verify_listener.local_endpoint().port()));
        worker.spawn(
            [this](boost::asio::yield_context yield)
            {
                try
                {
                    for (;;)
                    {
                        tcp::socket socket(worker.io());
                        error_code error;
                        verify_listener.async_accept(socket, yield[error]);
                        if (error == boost::asio::error::operation_aborted)
                        {
                            return;
                        }
                        require(!error, "fake verify accept");
                        boost::beast::flat_buffer buffer;
                        http::request<http::string_body> incoming;
                        http::async_read(socket, buffer, incoming, yield[error]);
                        require(!error && incoming.method() == http::verb::post && incoming.target() == "/internal/verify",
                                "fake verify HTTP request");
                        ++verify_count;
                        const auto identity = boost::json::parse(incoming.body()).as_object();
                        require(identity.size() == 3 && identity.at("stream_id").as_string() == stream_id &&
                                    identity.at("token").as_string() == token && identity.at("operation").as_string() == "play",
                                "fake verify exact identity");
                        const bool authorized = !deny && pending;
                        if (authorized)
                        {
                            pending = false;
                        }
                        http::response<http::empty_body> reply(authorized ? http::status::ok : http::status::forbidden, 11);
                        reply.keep_alive(false);
                        reply.prepare_payload();
                        http::async_write(socket, reply, yield[error]);
                        require(!error, "fake verify response");
                    }
                }
                catch (...)
                {
                    verification_failure = std::current_exception();
                }
            });
        runner = std::jthread([this]() { worker.run(); });
    }

    ~fixture() { cleanup(); }

    void add_playback()
    {
        // startup 没有端点 getter；预留、释放、绑定之间若被外部抢占，应明确失败而非静默重试。
        tcp::acceptor reservation(peer_io, {boost::asio::ip::address_v4::loopback(), 0});
        device_endpoint = reservation.local_endpoint();
        reservation.close();
        receiver = std::make_shared<gb28181_playback_session>(worker, stream_id, 96, 0x12345678U);
        lifetime = receiver;
        on(worker,
           [this]()
           {
               require(session_registry::instance().add_receiver_session(stream_id, receiver), "register playback receiver");
               require(receiver->startup(device_endpoint.address(), device_endpoint.port()), "playback listen");
           });
    }

    std::unique_ptr<tcp::socket> connect(tcp::endpoint endpoint)
    {
        auto socket = std::make_unique<tcp::socket>(peer_io);
        require(!peer_operation(peer_io, *socket, [&](auto done) { socket->async_connect(endpoint, [done](error_code error) { done(error, 0); }); }),
                "peer connect");
        return socket;
    }

    template <typename Bytes>
    void write(tcp::socket& socket, const Bytes& bytes)
    {
        require(!peer_operation(peer_io, socket, [&](auto done) { boost::asio::async_write(socket, boost::asio::buffer(bytes), done); }),
                "peer request write");
    }

    std::unique_ptr<tcp::socket> start(const std::string& raw)
    {
        auto listener = std::make_shared<tcp::acceptor>(worker.io(), tcp::endpoint{boost::asio::ip::address_v4::loopback(), 0});
        // 异常退出也由 owner 关闭尚未完成 accept 的局部 listener，不让它阻塞 worker 排空。
        boost::scope::scope_exit close_listener(
            [executor = worker.io().get_executor(), listener]()
            {
                boost::asio::post(executor,
                                  [listener]()
                                  {
                                      error_code ignored;
                                      listener->close(ignored);
                                  });
            });
        auto incoming = std::make_shared<tcp::socket>(worker.io());
        auto result = std::make_shared<std::promise<void>>();
        auto future = result->get_future();
        boost::asio::post(worker.io(),
                          [listener, incoming, result]()
                          {
                              listener->async_accept(*incoming,
                                                     [listener, incoming, result](error_code error)
                                                     {
                                                         if (error)
                                                         {
                                                             result->set_exception(std::make_exception_ptr(std::runtime_error("HTTP accept")));
                                                         }
                                                         else
                                                         {
                                                             result->set_value();
                                                         }
                                                     });
                          });
        auto peer = connect(listener->local_endpoint());
        ready(future);
        write(*peer, raw);
        on(worker,
           [this, incoming, size = raw.size()]()
           {
               error_code error;
               const auto available = incoming->available(error);
               require(!error && available >= size, "entire raw request preloaded before HTTP startup");
               auto http_session = std::make_shared<media_server::http_session>(worker, std::move(*incoming), application_config);
               http_lifetimes.push_back(http_session);
               http_session->startup();
           });
        return peer;
    }

    void inspect(unsigned expected_count, bool expected_pending)
    {
        on(worker,
           [this, expected_count, expected_pending]()
           {
               if (verification_failure)
               {
                   std::rethrow_exception(verification_failure);
               }
               require(verify_count == expected_count && pending == expected_pending, "verify count / token consumption");
           });
    }

    void reject_verification(bool value)
    {
        on(worker, [this, value]() { deny = value; });
    }

    void finish()
    {
        cleanup();
        require(!session_registry::instance().find_receiver_session(stream_id), "playback registry cleanup");
        require(lifetime.expired(), "playback lifetime cleanup");
        for (const auto& http_lifetime : http_lifetimes)
        {
            require(http_lifetime.expired(), "HTTP request lifetime cleanup");
        }
        if (verification_failure)
        {
            std::rethrow_exception(verification_failure);
        }
    }

    worker_context worker;
    boost::asio::io_context peer_io;
    std::shared_ptr<gb28181_playback_session> receiver;
    tcp::endpoint device_endpoint;

   private:
    void cleanup() noexcept
    {
        if (!worker.stop_requested())
        {
            // socket 操作和 shutdown subscription 清理均由 owner 执行。
            try
            {
                on(worker,
                   [this]()
                   {
                       error_code ignored;
                       verify_listener.close(ignored);
                       if (receiver)
                       {
                           receiver->shutdown();
                       }
                   });
            }
            catch (...)
            {
            }
        }
        receiver.reset();
        worker.request_stop();
        if (runner.joinable())
        {
            runner.join();
        }
    }

    config application_config;
    tcp::acceptor verify_listener;
    std::jthread runner;
    bool deny{};
    bool pending{true};
    unsigned verify_count{};
    std::exception_ptr verification_failure;
    std::weak_ptr<gb28181_playback_session> lifetime;
    std::vector<std::weak_ptr<http_session>> http_lifetimes;
};

struct response
{
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;

    void read(fixture& f, tcp::socket& socket, http::status expected, bool head = false, bool header_only = false)
    {
        parser.skip(head);
        const auto error = peer_operation(f.peer_io,
                                          socket,
                                          [&](auto done)
                                          {
                                              if (header_only)
                                              {
                                                  http::async_read_header(socket, buffer, parser, done);
                                              }
                                              else
                                              {
                                                  http::async_read(socket, buffer, parser, done);
                                              }
                                          });
        require(!error && parser.get().result() == expected, "HTTP route status");
        if (!header_only)
        {
            require(parser.is_done(), "complete HTTP rejection");
        }
    }
};

void prechecks()
{
    fixture f;
    f.add_playback();
    const std::array<std::string, 16> invalid{{
        "/play/gb28181",
        "/play/gb28181/",
        "/play/gb28181/" + stream_id,
        path + "/extra",
        path + "/",
        path + "?",
        path + "?x=1",
        path + "#fragment",
        "/play/gb28181/./" + stream_id + "/" + token + ".ps",
        "/play/gb28181/" + stream_id + "/%62" + token.substr(1) + ".ps",
        "/play/gb28181/" + stream_id + "/B" + token.substr(1) + ".ps",
        "/play/gb28181/" + stream_id + "/" + token + ".PS",
        "/play/gb28181/" + stream_id + "//" + token + ".ps",
        "/play/gb28181/" + stream_id + "/.ps",
        "/play/gb28181/not-a-stream/" + token + ".ps",
        "/play/gb28181/" + stream_id + "/" + token.substr(1) + ".ps",
    }};
    for (const auto& target : invalid)
    {
        auto socket = f.start(request(target));
        response result;
        result.read(f, *socket, http::status::bad_request);
        f.inspect(0, true);
    }
    for (const auto& raw : {request(path, "GET", "HTTP/1.0"), request(path, "GET", "HTTP/1.1", "x")})
    {
        auto socket = f.start(raw);
        response result;
        result.read(f, *socket, http::status::bad_request);
        f.inspect(0, true);
    }
    for (const auto method : {"POST", "HEAD"})
    {
        auto socket = f.start(request(path, method));
        response result;
        result.read(f, *socket, http::status::method_not_allowed, std::string_view(method) == "HEAD");
        require(result.parser.get()[http::field::allow] == "GET", "405 Allow GET");
        f.inspect(0, true);
    }
    // 此用例仅针对预置回环数据和锁定 Beast，不声称任意分包或晚到字节都会进入 flat_buffer。
    auto socket = f.start(request(path) + "EXTRA");
    response trailing;
    trailing.read(f, *socket, http::status::bad_request);
    f.inspect(0, true);
    require(session_registry::instance().find_receiver_session(stream_id).get() == f.receiver.get(), "prechecks preserve receiver");
    f.finish();
}

void missing_or_wrong_receiver()
{
    fixture f;
    auto socket = f.start(request(path));
    response missing;
    missing.read(f, *socket, http::status::not_found);
    f.inspect(0, true);
    auto other = std::make_shared<other_receiver>();
    require(session_registry::instance().add_receiver_session(stream_id, other), "register non-playback receiver");
    try
    {
        socket = f.start(request(path));
        response wrong;
        wrong.read(f, *socket, http::status::not_found);
        f.inspect(0, true);
    }
    catch (...)
    {
        session_registry::instance().remove_receiver_session(stream_id, *other);
        throw;
    }
    session_registry::instance().remove_receiver_session(stream_id, *other);
    f.finish();
}

void denied_then_success_and_replay()
{
    fixture f;
    f.add_playback();
    f.reject_verification(true);
    auto rejected_socket = f.start(request(path));
    response rejected;
    rejected.read(f, *rejected_socket, http::status::forbidden);
    f.inspect(1, true);
    f.reject_verification(false);
    auto viewer = f.start(request(path));
    response accepted;
    accepted.read(f, *viewer, http::status::ok, false, true);
    require(accepted.parser.chunked() && accepted.parser.get()[http::field::content_type] == "video/mpeg", "raw PS accepted header");
    f.inspect(2, false);
    // 保持首个 viewer 和 receiver 存活；否则 404 不能证明信令拒绝了已消费 token。
    auto replay_socket = f.start(request(path));
    response replay;
    replay.read(f, *replay_socket, http::status::forbidden);
    f.inspect(3, false);
    auto device = f.connect(f.device_endpoint);
    const std::vector<std::uint8_t> bytes{0, 16, 0x80, 96, 0, 1, 0, 0, 0, 1, 0x12, 0x34, 0x56, 0x78, 'P', 'S', '\0', 'X'};
    f.write(*device, bytes);
    error_code error;
    device->shutdown(tcp::socket::shutdown_send, error);
    require(!error, "device FIN");
    require(!peer_operation(f.peer_io, *viewer, [&](auto done) { http::async_read(*viewer, accepted.buffer, accepted.parser, done); }),
            "authorized viewer body");
    require(accepted.parser.is_done() && accepted.parser.get().body() == std::string("PS\0X", 4),
            "denial did not attach; subsequent success owns exact payload");
    f.finish();
}
}    // namespace

int main()
{
    const std::array<std::pair<const char*, void (*)()>, 3> cases{{
        {"local_prechecks", prechecks},
        {"missing_or_wrong_receiver", missing_or_wrong_receiver},
        {"denied_then_success_and_replay", denied_then_success_and_replay},
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
    return 0;
}
