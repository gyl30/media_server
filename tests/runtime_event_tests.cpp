#include <chrono>
#include <future>
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
    config cfg{};
    cfg.signaling_url = std::move(url);
    cfg.server_id = "media-1";
    cfg.control_url = "http://127.0.0.1:8080";
    cfg.media_ip = "127.0.0.1";
    signaling_client::instance().configure(cfg, "instance-1", std::chrono::milliseconds(500));
    boost::asio::spawn(worker.io(), [](boost::asio::yield_context yield) { signaling_client::instance().run(yield); }, boost::asio::detached);
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

bool wait_runtime_events(const test::publish_claim_test_server& server, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime_events(server).size() < count && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return runtime_events(server).size() >= count;
}

void test_rtsp_pull_runtime_failure_events()
{
    worker_context worker;
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
    const bool received = wait_runtime_events(server, 3U);

    const auto events = runtime_events(server);
    const bool released = !stream_registry::instance().take_receiver_session("live/runtime-events");
    session->shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto final_event_count = runtime_events(server).size();
    worker.stop();
    runner.join();

    require(received, "runtime event count");
    require(events.size() == 3U, "runtime failure emits fact and stopped once");
    const auto& starting = events[0];
    const auto& failure = events[1];
    const auto& stopped = events[2];
    require_identity(starting);
    require(starting.at("state") == "starting", "runtime event source starting");
    require(starting.at("stage") == "resolving" && !starting.contains("error"), "runtime event starting fields");
    require_identity(failure);
    require(failure.at("state") == "runtime_error" && failure.contains("error") && !failure.contains("stage"), "runtime event failure fact");
    require_identity(stopped);
    require(stopped.at("state") == "stopped" && !stopped.contains("error") && !stopped.contains("stage"), "runtime event stopped lifecycle");
    require(released, "runtime event pull releases identity");
    require(final_event_count == 3U, "late shutdown does not duplicate stopped");
}

void test_rtsp_pull_ordinary_shutdown_reports_stopped()
{
    worker_context worker;
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
    std::promise<void> started;
    auto ready = started.get_future();
    std::jthread runner([&worker]() { worker.run(); });
    boost::asio::post(worker.io(),
                      [session, &started]()
                      {
                          require(session->startup(), "runtime event shutdown startup");
                          started.set_value();
                      });
    ready.get();
    const bool received = wait_runtime_events(server, 1U);
    session->shutdown();
    session->shutdown();
    const bool stopped = wait_runtime_events(server, 3U);
    const auto events = runtime_events(server);
    const bool released = !stream_registry::instance().take_receiver_session("live/runtime-events");
    worker.stop();
    runner.join();

    require(received, "runtime event count");
    require(stopped && events.size() == 3U && events[0].at("state") == "starting" && events[1].at("state") == "stopped" &&
                events[2].at("state") == "runtime_error" && events[2].contains("error"),
            "ordinary shutdown emits stopped followed by the canceled transport fact");
    require(released, "runtime shutdown releases identity");
}

void test_runtime_event_strings()
{
    require(to_string(event_kind::source) == "source", "runtime source kind string");
    require(to_string(event_kind::publisher) == "publisher", "runtime publisher kind string");
    require(to_string(event_kind::output) == "output", "runtime output kind string");
    require(to_string(event_protocol::http_flv) == "http-flv", "HTTP-FLV runtime event protocol string");
    require(to_string(event_protocol::hls) == "hls", "HLS runtime event protocol string");
    require(to_string(event_protocol::whep) == "whep", "runtime event protocol string");
    require(to_string(event_state::streaming) == "streaming", "runtime event state string");
    require(to_string(event_state::stop_requested) == "stop_requested", "runtime requested fact string");
    require(to_string(event_state::remote_closed) == "remote_closed", "runtime remote fact string");
    require(to_string(event_state::timeout) == "timeout", "runtime timeout fact string");
    require(to_string(event_state::protocol_error) == "protocol_error", "runtime protocol error fact string");
    require(to_string(event_state::runtime_error) == "runtime_error", "runtime error fact string");
}

void test_make_event()
{
    const auto event = make_event(event_kind::source,
                                  event_protocol::rtsp,
                                  event_state::runtime_error,
                                  stream_id,
                                  "live/runtime-events",
                                  source_id,
                                  "transport",
                                  "connection_failed");
    require(event.kind == event_kind::source && event.protocol == event_protocol::rtsp && event.state == event_state::runtime_error,
            "event enum fields");
    require(event.stream_id == stream_id && event.stream_name == "live/runtime-events" && event.source_id == source_id, "event identity fields");
    require(event.stage == "transport" && event.error == "connection_failed", "event diagnostic fields");

    const auto stopped = make_event(event_kind::output, event_protocol::whep, event_state::stopped, stream_id, "live/runtime-events");
    require(!stopped.source_id && !stopped.stage && !stopped.error, "empty optional event fields");
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
        else if (test == "creation")
        {
            media_server::test_make_event();
        }
        else if (test == "failure")
        {
            media_server::test_rtsp_pull_runtime_failure_events();
        }
        else if (test == "ordinary_shutdown")
        {
            media_server::test_rtsp_pull_ordinary_shutdown_reports_stopped();
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
