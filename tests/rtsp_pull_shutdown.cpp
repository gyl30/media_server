#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <netdb.h>

#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/core/session_registry.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"

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
constexpr std::string_view test_host = "pull-shutdown.test";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void close_during_resolve(bool replace)
{
    {
        std::scoped_lock lock(resolve_mutex);
        entered_resolve = false;
        release_resolve = false;
        returned_resolve = false;
    }
    worker_context worker;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor upstream(worker.io(), {address, 0});
    const auto port = upstream.local_endpoint().port();
    const std::string stream_id = "verify/rtsp-resolve-shutdown";
    auto receiver = std::make_shared<rtsp_pull_session>(
        worker, rtsp_pull_config{.stream_id = stream_id,
                                 .url = "rtsp://" + std::string(test_host) + ":" + std::to_string(port) + "/source",
                                 .host = std::string(test_host),
                                 .port = port,
                                 .username = {},
                                 .password = {}});
    const std::weak_ptr<rtsp_pull_session> lifetime = receiver;
    std::shared_ptr<rtsp_pull_session> replacement;
    std::shared_ptr<tcp::socket> peer;
    bool connected{};
    bool described{};
    std::string request;
    boost::scope::scope_exit cleanup([&]()
    {
        {
            std::scoped_lock lock(resolve_mutex);
            release_resolve = true;
        }
        resolve_event.notify_all();
        boost::system::error_code error;
        upstream.close(error);
        if (peer)
        {
            peer->close(error);
        }
        if (const auto remaining = lifetime.lock())
        {
            remaining->shutdown();
        }
        if (replacement)
        {
            session_registry::instance().remove_receiver_session(stream_id, *replacement);
            replacement->shutdown();
            replacement.reset();
        }
        receiver.reset();
        worker.request_stop();
        worker.io().restart();
        worker.io().run_for(1s);
    });
    upstream.async_accept([&](boost::system::error_code error, tcp::socket socket)
    {
        if (error)
        {
            return;
        }
        connected = true;
        peer = std::make_shared<tcp::socket>(std::move(socket));
        boost::asio::async_read_until(*peer, boost::asio::dynamic_buffer(request, 8192), "\r\n\r\n",
                                     [&](boost::system::error_code read_error, std::size_t)
                                     {
                                         described = !read_error && request.starts_with("DESCRIBE ");
                                         boost::system::error_code close_error;
                                         peer->close(close_error);
                                     });
    });
    require(session_registry::instance().add_receiver_session(stream_id, receiver), "pull shutdown registration failed");
    boost::asio::post(worker.io(), [receiver]() { receiver->startup(); });
    worker.io().poll();
    {
        std::unique_lock lock(resolve_mutex);
        require(resolve_event.wait_for(lock, 3s, []() { return entered_resolve; }), "resolver did not enter controlled getaddrinfo");
    }
    receiver->shutdown();
    std::promise<void> cleaned;
    auto cleanup_barrier = cleaned.get_future();
    boost::asio::post(worker.io(), [&]() { cleaned.set_value(); });
    worker.io().poll();
    require(cleanup_barrier.wait_for(0s) == std::future_status::ready &&
                !session_registry::instance().find_receiver_session(stream_id),
            "pull safe_shutdown did not run before resolver release");
    receiver.reset();
    if (replace)
    {
        replacement = std::make_shared<rtsp_pull_session>(
            worker, rtsp_pull_config{.stream_id = stream_id,
                                     .url = "rtsp://127.0.0.1:" + std::to_string(port) + "/replacement",
                                     .host = "127.0.0.1",
                                     .port = port,
                                     .username = {},
                                     .password = {}});
        require(session_registry::instance().add_receiver_session(stream_id, replacement), "replacement pull registration failed");
    }
    {
        std::scoped_lock lock(resolve_mutex);
        release_resolve = true;
    }
    resolve_event.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!lifetime.expired() && !described && std::chrono::steady_clock::now() < deadline)
    {
        worker.io().run_one_for(deadline - std::chrono::steady_clock::now());
    }
    {
        std::scoped_lock lock(resolve_mutex);
        require(returned_resolve, "controlled resolver did not return");
    }
    std::cout << "RTSP pull after cleanup replacement=" << replace << ": connected=" << connected << " describe=" << described
              << " expired=" << lifetime.expired() << '\n';
    require(!connected && !described, "closed RTSP pull reopened its socket and contacted upstream after resolver completion");
    require(lifetime.expired(), "closed RTSP pull retained coroutine ownership after resolver completion");
    require(session_registry::instance().find_receiver_session(stream_id) == replacement,
            "old pull continuation changed replacement receiver ownership");
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
        close_during_resolve(false);
        close_during_resolve(true);
        std::cout << "RTSP pull shutdown during active resolver: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
