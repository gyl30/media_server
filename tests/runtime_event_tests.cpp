#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/post.hpp>
#include <boost/json/parse.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/rtsp/rtsp_pull_session.h"
#include "tests/clients/publish_claim_test_server.h"

namespace media_server
{
namespace
{

constexpr char stream_id[] = "550e8400-e29b-41d4-a716-446655440000";
constexpr char source_id[] = "10000000-0000-4000-8000-000000000001";

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string{message});
    }
}

void configure_reporting(worker_context& worker, std::string url)
{
    signaling_client::instance().configure({
        .signaling_url = std::move(url),
        .server_id = "media-1",
        .instance_id = "instance-1",
        .control_url = "http://127.0.0.1:8080",
        .media_ip = "127.0.0.1",
        .rtmp_port = 1935,
        .rtsp_port = 8554,
        .http_port = 8080,
        .heartbeat_interval = std::chrono::milliseconds(10),
    });
    boost::asio::spawn(
        worker.io(), [](boost::asio::yield_context yield) { signaling_client::instance().run(yield, []() {}); }, boost::asio::detached);
}

void require_identity(const boost::json::object& event)
{
    require(event.at("stream_id") == stream_id, "runtime event stream id");
    require(event.at("stream_name") == "live/runtime-events", "runtime event stream name");
    require(event.at("source_id") == source_id, "runtime event source id");
    require(event.at("kind") == "source", "runtime event kind");
    require(event.at("protocol") == "rtsp", "runtime event protocol");
}

std::vector<boost::json::object> runtime_events(const test::publish_claim_test_server& server)
{
    std::vector<boost::json::object> events;
    for (const auto& request : server.requests("/internal/runtime-events"))
    {
        const auto body = boost::json::parse(request.body).as_object();
        require(body.at("server_id") == "media-1" && body.at("instance_id") == "instance-1", "runtime event batch identity");
        for (const auto& event : body.at("events").as_array())
        {
            events.push_back(event.as_object());
        }
    }
    return events;
}

void wait_runtime_events(const test::publish_claim_test_server& server, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime_events(server).size() < count && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(runtime_events(server).size() >= count, "runtime event count");
}

void test_rtsp_pull_runtime_failure_events()
{
    worker_context worker;
    stream_registry::instance().clear();
    test::publish_claim_test_server server;
    configure_reporting(worker, server.url());

    boost::asio::ip::tcp::acceptor endpoint(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = endpoint.local_endpoint().port();
    endpoint.close();

    auto session = std::make_shared<rtsp_pull_session>(worker,
                                                       stream_id,
                                                       source_id,
                                                       "live/runtime-events",
                                                       "rtsp://127.0.0.1:" + std::to_string(port) + "/source",
                                                       "",
                                                       "",
                                                       std::chrono::milliseconds{200},
                                                       std::chrono::milliseconds{200},
                                                       1024U * 1024U);
    require(stream_registry::instance().add_receiver_session("live/runtime-events", session), "runtime event receiver reservation");
    boost::asio::post(worker.io(), [session]() { require(session->startup(), "runtime event pull startup"); });

    std::jthread runner([&worker]() { worker.run(); });
    wait_runtime_events(server, 2U);

    const auto events = runtime_events(server);
    require(events.size() == 2U, "runtime event stopped once");
    const auto& starting = events[0];
    const auto& stopped = events[1];
    require_identity(starting);
    require(starting.at("state") == "starting", "runtime event source starting");
    require(starting.at("stage") == "resolving" && !starting.contains("end_reason") && !starting.contains("error"), "runtime event starting fields");
    require_identity(stopped);
    require(stopped.at("state") == "stopped", "runtime event source runtime failure");
    require(stopped.at("end_reason") == "runtime_error" && stopped.contains("error"), "runtime event failure fields");
    require(!stream_registry::instance().take_receiver_session("live/runtime-events"), "runtime event pull releases identity");

    session->shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(runtime_events(server).size() == 2U, "runtime event repeated shutdown ignored");
    worker.stop();
    runner.join();
    stream_registry::instance().clear();
}

void test_rtsp_pull_first_shutdown_reason()
{
    worker_context worker;
    stream_registry::instance().clear();
    test::publish_claim_test_server server;
    configure_reporting(worker, server.url());

    boost::asio::ip::tcp::acceptor endpoint(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = endpoint.local_endpoint().port();
    auto session = std::make_shared<rtsp_pull_session>(worker,
                                                       stream_id,
                                                       source_id,
                                                       "live/runtime-events",
                                                       "rtsp://127.0.0.1:" + std::to_string(port) + "/source",
                                                       "",
                                                       "",
                                                       std::chrono::seconds{5},
                                                       std::chrono::seconds{5},
                                                       1024U * 1024U);
    require(stream_registry::instance().add_receiver_session("live/runtime-events", session), "runtime event shutdown reservation");
    boost::asio::post(worker.io(),
                      [session]()
                      {
                          require(session->startup(), "runtime event shutdown startup");
                          session->shutdown(runtime_end_reason::server_shutdown);
                          session->shutdown(runtime_end_reason::requested);
                      });

    std::jthread runner([&worker]() { worker.run(); });
    wait_runtime_events(server, 2U);
    const auto events = runtime_events(server);
    require(events.size() == 2U, "runtime shutdown transitions once");
    const auto& stopped = events[1];
    require(stopped.at("state") == "stopped", "runtime shutdown terminal event");
    require(stopped.at("end_reason") == "server_shutdown" && !stopped.contains("error"), "runtime shutdown first reason wins");
    require(!stream_registry::instance().take_receiver_session("live/runtime-events"), "runtime shutdown releases identity");
    worker.stop();
    runner.join();
    stream_registry::instance().clear();
}

void test_runtime_event_strings()
{
    require(to_string(runtime_kind::source) == "source", "runtime source kind string");
    require(to_string(runtime_kind::publisher) == "publisher", "runtime publisher kind string");
    require(to_string(runtime_kind::output) == "output", "runtime output kind string");
    require(to_string(runtime_protocol::whep) == "whep", "runtime event protocol string");
    require(to_string(runtime_state::streaming) == "streaming", "runtime event state string");
    require(to_string(runtime_end_reason::server_shutdown) == "server_shutdown", "runtime event end reason string");
}

}    // namespace
}    // namespace media_server

int main(int argc, char** argv)
{
    try
    {
        media_server::require(argc == 2, "runtime event test case required");
        const std::string_view test{argv[1]};
        if (test == "strings")
        {
            media_server::test_runtime_event_strings();
        }
        else if (test == "failure")
        {
            media_server::test_rtsp_pull_runtime_failure_events();
        }
        else if (test == "shutdown_reason")
        {
            media_server::test_rtsp_pull_first_shutdown_reason();
        }
        else
        {
            throw std::runtime_error("unknown runtime event test case");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
