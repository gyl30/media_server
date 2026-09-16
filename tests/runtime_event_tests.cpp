#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/post.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/http/event_reporter.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_pull_session.h"

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

void capture_events(std::vector<runtime_event>& events, worker_context& worker, bool& owner_worker)
{
    event_reporter::instance().configure_handler("media-1",
                                                 "instance-1",
                                                 [&events, &worker, &owner_worker](runtime_event event)
                                                 {
                                                     owner_worker = owner_worker && worker.io().get_executor().running_in_this_thread();
                                                     events.push_back(std::move(event));
                                                 });
}

void require_identity(const runtime_event& event)
{
    require(event.server_id == "media-1", "runtime event server id");
    require(event.instance_id == "instance-1", "runtime event instance id");
    require(event.stream_id == stream_id, "runtime event stream id");
    require(event.stream_name == "live/runtime-events", "runtime event stream name");
    require(event.source_id == source_id, "runtime event source id");
    require(event.kind == runtime_kind::source, "runtime event kind");
    require(event.protocol == runtime_protocol::rtsp, "runtime event protocol");
}

void test_rtsp_pull_runtime_failure_events()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    boost::asio::ip::tcp::acceptor endpoint(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = endpoint.local_endpoint().port();
    endpoint.close();

    std::vector<runtime_event> events;
    bool owner_worker = true;
    capture_events(events, worker, owner_worker);
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
    require(streams.add_receiver_session("live/runtime-events", session), "runtime event receiver reservation");
    boost::asio::post(worker.io(), [session]() { require(session->startup(), "runtime event pull startup"); });

    worker.release_work();
    worker.io().run();

    require(events.size() == 2U, "runtime event stopped once");
    require(owner_worker, "runtime events emitted on owner worker");
    require_identity(events[0]);
    require(events[0].kind == runtime_kind::source && events[0].state == runtime_state::starting, "runtime event source starting");
    require(events[0].stage == "resolving" && !events[0].end_reason && !events[0].error, "runtime event starting fields");
    require_identity(events[1]);
    require(events[1].kind == runtime_kind::source && events[1].state == runtime_state::stopped, "runtime event source runtime failure");
    require(events[1].end_reason == runtime_end_reason::runtime_error && events[1].error.has_value(), "runtime event failure fields");
    require(!streams.take_receiver_session("live/runtime-events"), "runtime event pull releases identity");

    session->shutdown();
    worker.io().restart();
    worker.io().run();
    require(events.size() == 2U, "runtime event repeated shutdown ignored");
    streams.clear();
}

void test_rtsp_pull_first_shutdown_reason()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    boost::asio::ip::tcp::acceptor endpoint(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = endpoint.local_endpoint().port();
    std::vector<runtime_event> events;
    bool owner_worker = true;
    capture_events(events, worker, owner_worker);
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
    require(streams.add_receiver_session("live/runtime-events", session), "runtime event shutdown reservation");
    boost::asio::post(worker.io(),
                      [session]()
                      {
                          require(session->startup(), "runtime event shutdown startup");
                          session->shutdown(runtime_end_reason::server_shutdown);
                          session->shutdown(runtime_end_reason::requested);
                      });

    worker.release_work();
    worker.io().run();
    require(owner_worker, "runtime shutdown events emitted on owner worker");
    require(events.size() == 2U, "runtime shutdown transitions once");
    require(events[1].kind == runtime_kind::source && events[1].state == runtime_state::stopped, "runtime shutdown terminal event");
    require(events[1].end_reason == runtime_end_reason::server_shutdown && !events[1].error, "runtime shutdown first reason wins");
    require(!streams.take_receiver_session("live/runtime-events"), "runtime shutdown releases identity");
    streams.clear();
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

int main()
{
    try
    {
        media_server::test_runtime_event_strings();
        std::cout << "[pass] runtime_event_strings\n";
        media_server::test_rtsp_pull_runtime_failure_events();
        std::cout << "[pass] rtsp_pull_runtime_failure_events\n";
        media_server::test_rtsp_pull_first_shutdown_reason();
        std::cout << "[pass] rtsp_pull_first_shutdown_reason\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
