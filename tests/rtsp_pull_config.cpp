#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <netdb.h>

#include <ada.h>
#include <boost/asio/read_until.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/core/stream_registry.h"
#include "media/http/rtsp_pull_http.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
std::atomic_uint default_port_target{};
std::atomic_uint default_port_resolves{};
constexpr std::string_view stream_id = "00000000-0000-0000-0000-000000000001";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

rtsp_pull_http_response create(worker_context& worker, boost::json::object body)
{
    rtsp_pull_http_request request(http::verb::post, "/rtsp/pull/create", 11);
    request.set(http::field::content_type, "application/json");
    request.body() = boost::json::serialize(body);
    const auto target = ada::parse<ada::url_aggregator>("http://localhost/rtsp/pull/create");
    return handle_rtsp_pull_request(request, worker, *target);
}

void invalid_requests()
{
    worker_context worker;
    for (const std::string_view url : {"http://127.0.0.1/source",
                                       "rtsps://127.0.0.1/source",
                                       "rtsp:///source",
                                       "rtsp://127.0.0.1:0/source",
                                       "rtsp://127.0.0.1:65536/source",
                                       "rtsp://127.0.0.1:invalid/source",
                                       "rtsp://user:password@127.0.0.1/source"})
    {
        const auto response = create(worker, {{"stream_id", stream_id}, {"url", url}});
        require(response.result() == http::status::bad_request, "invalid RTSP URL accepted");
        require(session_registry::instance().receivers().empty(), "invalid RTSP URL registered a receiver");
        require(!stream_registry::instance().find(stream_id), "invalid RTSP URL published a source");
    }
    for (const auto& body : {boost::json::object{{"stream_id", stream_id}, {"url", "rtsp://127.0.0.1/source"}, {"password", "test-only"}},
                             boost::json::object{{"stream_id", stream_id}, {"url", "rtsp://127.0.0.1/source"}, {"username", 123}},
                             boost::json::object{{"stream_id", stream_id}, {"url", "rtsp://127.0.0.1/source"}, {"extra", true}}})
    {
        require(create(worker, body).result() == http::status::bad_request, "invalid RTSP request parameters accepted");
        require(session_registry::instance().receivers().empty(), "invalid RTSP parameters registered a receiver");
    }
    worker.request_stop();
    worker.io().run_for(5s);
    require(worker.io().stopped(), "invalid RTSP request retained work");
    std::cout << "RTSP invalid URL and credentials: PASS\n";
}

void registration_conflicts()
{
    worker_context worker;
    auto source = std::make_shared<media_stream>(std::string(stream_id), worker);
    require(source->set_tracks(
                {{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}}),
            "source tracks rejected");
    require(stream_registry::instance().add(source), "source registration failed");
    std::shared_ptr<rtsp_pull_session> receiver;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (source)
            {
                stream_registry::instance().remove(*source);
                source->end();
                source.reset();
            }
            if (auto current = session_registry::instance().take_receiver_session(stream_id))
            {
                current->shutdown();
            }
            receiver.reset();
            worker.request_stop();
            worker.io().restart();
            worker.io().poll();
        });
    const boost::json::object request{{"stream_id", stream_id}, {"url", "rtsp://127.0.0.1:12345/source"}};
    require(create(worker, request).result() == http::status::conflict, "existing source did not reject RTSP pull");
    worker.io().poll();
    require(stream_registry::instance().find(stream_id) == source && session_registry::instance().receivers().empty(),
            "source conflict changed existing ownership");
    stream_registry::instance().remove(*source);
    source->end();
    source.reset();

    receiver = std::make_shared<rtsp_pull_session>(worker,
                                                   rtsp_pull_config{.stream_id = std::string(stream_id),
                                                                    .url = "rtsp://127.0.0.1:12345/source",
                                                                    .host = "127.0.0.1",
                                                                    .port = 12345,
                                                                    .username = {},
                                                                    .password = {}});
    require(session_registry::instance().add_receiver_session(std::string(stream_id), receiver), "receiver registration failed");
    require(create(worker, request).result() == http::status::conflict, "existing receiver did not reject RTSP pull");
    worker.io().poll();
    require(session_registry::instance().find_receiver_session(stream_id) == receiver, "failed registration cleanup removed the existing receiver");
    const std::weak_ptr<rtsp_pull_session> lifetime = receiver;
    auto detached = session_registry::instance().take_receiver_session(stream_id);
    detached->shutdown();
    detached.reset();
    receiver.reset();
    worker.request_stop();
    worker.io().run_for(5s);
    require(lifetime.expired() && session_registry::instance().receivers().empty(), "conflict test retained receiver resources");
    std::cout << "RTSP source and receiver registration conflicts: PASS\n";
}

void capture_describe(bool ipv6, bool default_port)
{
    worker_context worker;
    const auto address =
        ipv6 ? boost::asio::ip::address(boost::asio::ip::address_v6::loopback()) : boost::asio::ip::address(boost::asio::ip::address_v4::loopback());
    tcp::acceptor upstream(worker.io(), {address, 0});
    const auto port = upstream.local_endpoint().port();
    const std::string host = ipv6 ? "[::1]" : "127.0.0.1";
    const std::string authority = host + (default_port ? "" : ":" + std::to_string(port));
    const std::string input = "RTSP://" + authority + "/unused/../source?test=1";
    const std::string expected = "DESCRIBE rtsp://" + authority + "/source?test=1 RTSP/1.0\r\n";
    const auto resolve_count_before = default_port_resolves.load();
    default_port_target.store(default_port ? port : 0);
    bool captured{};
    std::exception_ptr failure;
    std::weak_ptr<session> lifetime;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            default_port_target.store(0);
            boost::system::error_code error;
            upstream.close(error);
            if (auto receiver = session_registry::instance().take_receiver_session(stream_id))
            {
                receiver->shutdown();
            }
            worker.request_stop();
            worker.io().restart();
            worker.io().poll();
        });
    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            boost::system::error_code error;
            auto socket = upstream.async_accept(yield[error]);
            require(!error, "RTSP upstream accept failed");
            boost::beast::tcp_stream peer(std::move(socket));
            peer.expires_after(3s);
            std::string request;
            boost::asio::async_read_until(peer, boost::asio::dynamic_buffer(request, 8192), "\r\n\r\n", yield[error]);
            require(!error, "RTSP upstream DESCRIBE read failed");
            require(request.starts_with(expected), "RTSP DESCRIBE did not use the normalized request URL");
            require(request.find("Accept: application/sdp\r\n") != std::string::npos, "RTSP DESCRIBE header missing");
            captured = true;
            peer.socket().close(error);
        },
        [&](std::exception_ptr error)
        {
            if (error)
            {
                failure = error;
                worker.request_stop();
            }
        });
    require(create(worker, {{"stream_id", stream_id}, {"url", input}}).result() == http::status::created, "valid RTSP pull creation failed");
    auto receiver = session_registry::instance().find_receiver_session(stream_id);
    require(receiver != nullptr, "RTSP pull was not registered before startup");
    lifetime = receiver;
    require(create(worker, {{"stream_id", stream_id}, {"url", input}}).result() == http::status::conflict &&
                session_registry::instance().find_receiver_session(stream_id) == receiver,
            "duplicate create replaced the active receiver");
    receiver.reset();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!captured || session_registry::instance().find_receiver_session(stream_id) || !lifetime.expired())
    {
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        require(std::chrono::steady_clock::now() < deadline && !worker.io().stopped(), "RTSP capture or peer-close cleanup exceeded deadline");
        worker.io().run_one_for(deadline - std::chrono::steady_clock::now());
    }
    if (default_port)
    {
        require(default_port_resolves.load() > resolve_count_before, "default RTSP port was not resolved as 554");
    }
    worker.request_stop();
    worker.io().run_for(deadline - std::chrono::steady_clock::now());
    require(worker.io().stopped() && !stream_registry::instance().find(stream_id), "RTSP capture retained resources or published an empty stream");
    std::cout << "RTSP " << (ipv6 ? "IPv6" : "IPv4") << (default_port ? " default" : " explicit")
              << " port, normalized DESCRIBE and peer-close cleanup: PASS\n";
}

void refused_connection()
{
    worker_context worker;
    tcp::socket reserved(worker.io());
    reserved.open(tcp::v4());
    reserved.bind({boost::asio::ip::address_v4::loopback(), 0});
    const auto url = "rtsp://127.0.0.1:" + std::to_string(reserved.local_endpoint().port()) + "/source";
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (auto receiver = session_registry::instance().take_receiver_session(stream_id))
            {
                receiver->shutdown();
            }
            worker.request_stop();
            worker.io().restart();
            worker.io().poll();
        });
    require(create(worker, {{"stream_id", stream_id}, {"url", url}}).result() == http::status::created,
            "asynchronous connection failure became a synchronous create failure");
    auto receiver = session_registry::instance().find_receiver_session(stream_id);
    require(receiver != nullptr, "failed connection was never registered");
    const std::weak_ptr<session> lifetime = receiver;
    receiver.reset();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session_registry::instance().find_receiver_session(stream_id) || !lifetime.expired())
    {
        require(std::chrono::steady_clock::now() < deadline && !worker.io().stopped(), "refused connection retained receiver resources");
        worker.io().run_one_for(deadline - std::chrono::steady_clock::now());
    }
    worker.request_stop();
    worker.io().run_for(deadline - std::chrono::steady_clock::now());
    require(worker.io().stopped() && !stream_registry::instance().find(stream_id), "failed connection published an empty stream");
    std::cout << "RTSP asynchronous refused connection cleanup: PASS\n";
}
}    // namespace

extern "C" int __real_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result);

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result)
{
    const auto target = default_port_target.load();
    if (target != 0 && service != nullptr && std::string_view(service) == "554")
    {
        ++default_port_resolves;
        const auto redirected_service = std::to_string(target);
        return __real_getaddrinfo(node, redirected_service.c_str(), hints, result);
    }
    return __real_getaddrinfo(node, service, hints, result);
}

int main()
{
    try
    {
        invalid_requests();
        registration_conflicts();
        capture_describe(false, false);
        capture_describe(false, true);
        capture_describe(true, false);
        capture_describe(true, true);
        refused_connection();
        require(session_registry::instance().receivers().empty(), "RTSP tests retained receiver registrations");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
