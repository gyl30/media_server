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

#include "media/rtmp/rtmp_event.h"
#include "media/rtsp/rtsp_event.h"
#include "media/webrtc/whep_event.h"
#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/gb28181/gb28181_event.h"
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
    require(stopped.at("end_reason") == "runtime_error" && stopped.contains("error") && !stopped.contains("stage"), "runtime event failure fields");
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
    session->shutdown(runtime_end_reason::server_shutdown, "first_error");
    session->shutdown(runtime_end_reason::requested, "late_error");
    wait_runtime_events(server, 2U);
    const auto events = runtime_events(server);
    require(events.size() == 2U, "runtime shutdown transitions once");
    const auto& stopped = events[1];
    require(stopped.at("state") == "stopped", "runtime shutdown terminal event");
    require(stopped.at("end_reason") == "server_shutdown" && stopped.at("error") == "first_error" && !stopped.contains("stage"),
            "runtime shutdown first reason wins");
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

void test_protocol_event_factories()
{
    const auto require_event =
        [](const runtime_event& event, runtime_kind kind, runtime_protocol protocol, runtime_state state, std::string_view stage)
    {
        require(event.kind == kind && event.protocol == protocol && event.state == state, "protocol event fixed identity");
        require(event.stream_id == stream_id && event.stream_name == "live/runtime-events", "protocol event stream identity");
        require(stage.empty() ? !event.stage.has_value() : event.stage && *event.stage == stage, "protocol event stage");
    };

    require_event(rtmp_event::publisher_starting(stream_id, "live/runtime-events"),
                  runtime_kind::publisher,
                  runtime_protocol::rtmp,
                  runtime_state::starting,
                  "publish");
    require_event(rtmp_event::publisher_streaming(stream_id, "live/runtime-events"),
                  runtime_kind::publisher,
                  runtime_protocol::rtmp,
                  runtime_state::streaming,
                  "streaming");
    const auto rtmp_stopped =
        rtmp_event::publisher_stopped(stream_id, "live/runtime-events", runtime_end_reason::runtime_error, "transport", "connection_failed");
    require_event(rtmp_stopped, runtime_kind::publisher, runtime_protocol::rtmp, runtime_state::stopped, "transport");
    require(rtmp_stopped.end_reason == runtime_end_reason::runtime_error && rtmp_stopped.error == "connection_failed", "rtmp stopped diagnostics");

    require_event(rtsp_event::publisher_starting(stream_id, "live/runtime-events"),
                  runtime_kind::publisher,
                  runtime_protocol::rtsp,
                  runtime_state::starting,
                  "announce");
    require_event(rtsp_event::publisher_streaming(stream_id, "live/runtime-events"),
                  runtime_kind::publisher,
                  runtime_protocol::rtsp,
                  runtime_state::streaming,
                  "streaming");
    const auto rtsp_publisher_stopped =
        rtsp_event::publisher_stopped(stream_id, "live/runtime-events", runtime_end_reason::protocol_error, "record", "invalid_record");
    require_event(rtsp_publisher_stopped, runtime_kind::publisher, runtime_protocol::rtsp, runtime_state::stopped, "record");
    require(rtsp_publisher_stopped.end_reason == runtime_end_reason::protocol_error && rtsp_publisher_stopped.error == "invalid_record",
            "rtsp publisher stopped diagnostics");

    const auto rtsp_starting = rtsp_event::source_starting(stream_id, "live/runtime-events", source_id);
    require_event(rtsp_starting, runtime_kind::source, runtime_protocol::rtsp, runtime_state::starting, "resolving");
    require(rtsp_starting.source_id == source_id, "rtsp source starting identity");
    const auto rtsp_streaming = rtsp_event::source_streaming(stream_id, "live/runtime-events", source_id);
    require_event(rtsp_streaming, runtime_kind::source, runtime_protocol::rtsp, runtime_state::streaming, "streaming");
    require(rtsp_streaming.source_id == source_id, "rtsp source streaming identity");
    const auto rtsp_stopped = rtsp_event::source_stopped(stream_id, "live/runtime-events", source_id, runtime_end_reason::timeout, "connect_timeout");
    require_event(rtsp_stopped, runtime_kind::source, runtime_protocol::rtsp, runtime_state::stopped, {});
    require(rtsp_stopped.source_id == source_id && rtsp_stopped.end_reason == runtime_end_reason::timeout && rtsp_stopped.error == "connect_timeout",
            "rtsp source stopped diagnostics");

    require_event(gb28181_event::source_starting(stream_id, "live/runtime-events", "listening"),
                  runtime_kind::source,
                  runtime_protocol::gb28181,
                  runtime_state::starting,
                  "listening");
    require_event(gb28181_event::source_streaming(stream_id, "live/runtime-events"),
                  runtime_kind::source,
                  runtime_protocol::gb28181,
                  runtime_state::streaming,
                  "streaming");
    const auto gb_source_stopped = gb28181_event::source_stopped(stream_id, "live/runtime-events", runtime_end_reason::remote, "receiver_closed");
    require_event(gb_source_stopped, runtime_kind::source, runtime_protocol::gb28181, runtime_state::stopped, {});
    require(gb_source_stopped.end_reason == runtime_end_reason::remote && gb_source_stopped.error == "receiver_closed",
            "gb28181 source stopped diagnostics");
    require_event(gb28181_event::output_starting(stream_id, "live/runtime-events"),
                  runtime_kind::output,
                  runtime_protocol::gb28181,
                  runtime_state::starting,
                  {});
    require_event(gb28181_event::output_streaming(stream_id, "live/runtime-events"),
                  runtime_kind::output,
                  runtime_protocol::gb28181,
                  runtime_state::streaming,
                  "streaming");
    const auto gb_output_stopped = gb28181_event::output_stopped(stream_id, "live/runtime-events", runtime_end_reason::requested);
    require_event(gb_output_stopped, runtime_kind::output, runtime_protocol::gb28181, runtime_state::stopped, {});
    require(gb_output_stopped.end_reason == runtime_end_reason::requested && !gb_output_stopped.error, "gb28181 output stopped diagnostics");

    require_event(
        whep_event::output_starting(stream_id, "live/runtime-events"), runtime_kind::output, runtime_protocol::whep, runtime_state::starting, "ice");
    require_event(whep_event::output_streaming(stream_id, "live/runtime-events"),
                  runtime_kind::output,
                  runtime_protocol::whep,
                  runtime_state::streaming,
                  "streaming");
    const auto whep_stopped = whep_event::output_stopped(stream_id, "live/runtime-events", runtime_end_reason::server_shutdown);
    require_event(whep_stopped, runtime_kind::output, runtime_protocol::whep, runtime_state::stopped, {});
    require(whep_stopped.end_reason == runtime_end_reason::server_shutdown && !whep_stopped.error, "whep stopped diagnostics");
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
        else if (test == "factories")
        {
            media_server::test_protocol_event_factories();
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
