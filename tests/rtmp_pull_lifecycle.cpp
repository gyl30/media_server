#include <array>
#include <mutex>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <netdb.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <condition_variable>

#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/core/session_registry.h"
#include "media/rtmp/rtmp_pull_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;

std::mutex resolve_mutex;
std::condition_variable resolve_event;
bool entered_resolve{};
bool release_resolve{};
bool returned_resolve{};
constexpr std::string_view test_host = "rtmp-pull.test";
constexpr std::string_view stream_id = "verify/rtmp-pull";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

rtmp_pull_config make_config(std::string host, std::uint16_t port)
{
    return rtmp_pull_config{.stream_id = std::string(stream_id),
                            .host = std::move(host),
                            .port = port,
                            .app = "live",
                            .stream = "cam",
                            .tc_url = "rtmp://127.0.0.1/live"};
}

// 解析期间关闭：迟到的解析结果不能再连接上游，也不能改变替换会话的注册。
void close_during_resolve()
{
    {
        std::scoped_lock lock(resolve_mutex);
        entered_resolve = false;
        release_resolve = false;
        returned_resolve = false;
    }
    worker_context worker;
    tcp::acceptor upstream(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = upstream.local_endpoint().port();
    auto receiver = std::make_shared<rtmp_pull_session>(worker, make_config(std::string(test_host), port));
    const std::weak_ptr<rtmp_pull_session> lifetime = receiver;
    bool connected{};
    boost::scope::scope_exit cleanup(
        [&]()
        {
            {
                std::scoped_lock lock(resolve_mutex);
                release_resolve = true;
            }
            resolve_event.notify_all();
            boost::system::error_code error;
            upstream.close(error);
            receiver.reset();
            worker.request_stop();
            worker.io().restart();
            worker.io().run_for(1s);
        });
    upstream.async_accept([&](boost::system::error_code error, tcp::socket) { connected = !error; });
    require(session_registry::instance().add_receiver_session(std::string(stream_id), receiver), "pull registration failed");
    boost::asio::post(worker.io(), [receiver]() { receiver->startup(); });
    worker.io().poll();
    {
        std::unique_lock lock(resolve_mutex);
        require(resolve_event.wait_for(lock, 3s, []() { return entered_resolve; }), "resolver did not enter controlled getaddrinfo");
    }
    receiver->shutdown();
    worker.io().poll();
    require(!session_registry::instance().find_receiver_session(stream_id), "pull safe_shutdown did not run before resolver release");
    receiver.reset();
    {
        std::scoped_lock lock(resolve_mutex);
        release_resolve = true;
    }
    resolve_event.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline)
    {
        worker.io().run_one_for(deadline - std::chrono::steady_clock::now());
    }
    worker.io().poll();
    {
        std::scoped_lock lock(resolve_mutex);
        require(returned_resolve, "controlled resolver did not return");
    }
    require(!connected, "closed RTMP pull connected upstream after resolver completion");
    require(lifetime.expired(), "closed RTMP pull retained coroutine ownership");
    require(!session_registry::instance().find_receiver_session(stream_id), "closed pull restored receiver ownership");
    std::cout << "RTMP pull shutdown during resolve: PASS\n";
}

// 已连接、握手未完成时关闭：上游连接被关闭，会话、注册和异步任务全部释放，不注册媒体源。
void close_during_handshake()
{
    worker_context worker;
    boost::asio::io_context peer_io;
    tcp::acceptor upstream(peer_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket peer(peer_io);
    auto receiver = std::make_shared<rtmp_pull_session>(worker, make_config("127.0.0.1", upstream.local_endpoint().port()));
    const std::weak_ptr<rtmp_pull_session> lifetime = receiver;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (auto remaining = session_registry::instance().take_receiver_session(stream_id))
            {
                remaining->shutdown();
            }
            receiver.reset();
            boost::system::error_code error;
            peer.close(error);
            worker.request_stop();
            worker.io().restart();
            worker.io().run_for(1s);
        });
    bool accepted{};
    upstream.async_accept(peer, [&](boost::system::error_code error) { accepted = !error; });
    require(session_registry::instance().add_receiver_session(std::string(stream_id), receiver), "pull registration failed");
    boost::asio::post(worker.io(), [receiver]() { receiver->startup(); });
    std::array<std::uint8_t, 1537> c0c1{};
    bool handshake{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!accepted)
    {
        require(std::chrono::steady_clock::now() < deadline, "upstream connection timed out");
        worker.io().poll();
        peer_io.run_one_for(10ms);
    }
    peer_io.restart();
    boost::asio::async_read(peer, boost::asio::buffer(c0c1), [&](boost::system::error_code error, std::size_t) { handshake = !error; });
    while (!handshake)
    {
        require(std::chrono::steady_clock::now() < deadline, "C0/C1 not received");
        worker.io().poll();
        peer_io.run_one_for(10ms);
    }
    require(c0c1[0] == 3, "unexpected RTMP version");

    auto detached = session_registry::instance().take_receiver_session(stream_id);
    require(detached == receiver, "receiver disappeared before control deletion");
    detached->shutdown();
    detached.reset();
    receiver.reset();
    worker.io().poll();
    std::array<char, 1> data{};
    boost::system::error_code error;
    peer.read_some(boost::asio::buffer(data), error);
    require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset, "shutdown retained upstream socket");
    worker.request_stop();
    worker.io().run_for(1s);
    require(worker.io().stopped() && lifetime.expired(), "handshake shutdown retained session or asynchronous work");
    require(!stream_registry::instance().find(stream_id), "unfinished RTMP pull registered a media source");
    std::cout << "RTMP pull shutdown during handshake: PASS\n";
}

// 上游接受连接后立即关闭：会话自行结束并释放注册。
void peer_close()
{
    worker_context worker;
    tcp::acceptor upstream(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    auto receiver = std::make_shared<rtmp_pull_session>(worker, make_config("127.0.0.1", upstream.local_endpoint().port()));
    const std::weak_ptr<rtmp_pull_session> lifetime = receiver;
    upstream.async_accept([](boost::system::error_code, tcp::socket socket) { socket.close(); });
    require(session_registry::instance().add_receiver_session(std::string(stream_id), receiver), "pull registration failed");
    receiver->startup();
    receiver.reset();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline)
    {
        worker.io().run_one_for(10ms);
    }
    require(lifetime.expired() && !session_registry::instance().find_receiver_session(stream_id), "peer close retained RTMP pull");
    worker.request_stop();
    worker.io().run_for(1s);
    std::cout << "RTMP pull upstream close cleanup: PASS\n";
}
}    // namespace

extern "C" int __real_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result);

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result)
{
    if (node == nullptr || std::string_view(node) != test_host)
    {
        return __real_getaddrinfo(node, service, hints, result);
    }
    {
        std::unique_lock lock(resolve_mutex);
        entered_resolve = true;
        resolve_event.notify_all();
        if (!resolve_event.wait_for(lock, std::chrono::seconds(5), []() { return release_resolve; }))
        {
            return EAI_AGAIN;
        }
    }
    const int status = __real_getaddrinfo("127.0.0.1", service, hints, result);
    {
        std::scoped_lock lock(resolve_mutex);
        returned_resolve = true;
    }
    resolve_event.notify_all();
    return status;
}

int main()
{
    try
    {
        close_during_resolve();
        close_during_handshake();
        peer_close();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
