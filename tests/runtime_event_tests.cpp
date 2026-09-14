#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

#include "media/core/runtime_event.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace media_server
{
namespace
{

constexpr char stream_id[] = "550e8400-e29b-41d4-a716-446655440000";

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string{message});
    }
}

runtime_event_emitter_ptr capture_events(std::vector<runtime_event>& events, worker_context& worker, bool& owner_worker)
{
    return std::make_shared<runtime_event_emitter>("media-1", "instance-1", [&events, &worker, &owner_worker](runtime_event event) {
        owner_worker = owner_worker && worker.io().get_executor().running_in_this_thread();
        events.push_back(std::move(event));
    });
}

void require_identity(const runtime_event& event, std::string_view source_id = {})
{
    require(event.server_id == "media-1", "runtime event server id");
    require(event.instance_id == "instance-1", "runtime event instance id");
    require(event.stream_id == stream_id, "runtime event stream id");
    require(event.stream_name == "live/runtime-events", "runtime event stream name");
    require(source_id.empty() ? !event.source_id : event.source_id == source_id, "runtime event source id");
    require(event.direction == runtime_direction::input, "runtime event direction");
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
    constexpr std::string_view source_id = "source-1";
    auto session = std::make_shared<rtsp_pull_session>(worker,
                                                       stream_id,
                                                       "live/runtime-events",
                                                       "rtsp://127.0.0.1:" + std::to_string(port) + "/source",
                                                       "",
                                                       "",
                                                       std::chrono::milliseconds{200},
                                                       std::chrono::milliseconds{200},
                                                       1024U * 1024U,
                                                       std::string{source_id},
                                                       capture_events(events, worker, owner_worker));
    require(streams.add_receiver_session("live/runtime-events", session), "runtime event receiver reservation");
    boost::asio::post(worker.io(), [session]() { require(session->startup(), "runtime event pull startup"); });

    worker.release_work();
    worker.io().run();

    require(events.size() == 2U, "runtime event stopped once");
    require(owner_worker, "runtime events emitted on owner worker");
    require_identity(events[0], source_id);
    require(events[0].type == runtime_event_type::source_started && events[0].state == runtime_state::starting,
            "runtime event source starting");
    require(events[0].stage == "resolving" && !events[0].end_reason && !events[0].error, "runtime event starting fields");
    require_identity(events[1], source_id);
    require(events[1].type == runtime_event_type::runtime_error && events[1].state == runtime_state::stopped,
            "runtime event source runtime failure");
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
    auto session = std::make_shared<rtsp_pull_session>(worker,
                                                       stream_id,
                                                       "live/runtime-events",
                                                       "rtsp://127.0.0.1:" + std::to_string(port) + "/source",
                                                       "",
                                                       "",
                                                       std::chrono::seconds{5},
                                                       std::chrono::seconds{5},
                                                       1024U * 1024U,
                                                       std::optional<std::string>{},
                                                       capture_events(events, worker, owner_worker));
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
    require(events[1].type == runtime_event_type::source_stopped && events[1].state == runtime_state::stopped,
            "runtime shutdown terminal event");
    require(events[1].end_reason == runtime_end_reason::server_shutdown && !events[1].error, "runtime shutdown first reason wins");
    require(!streams.take_receiver_session("live/runtime-events"), "runtime shutdown releases identity");
    streams.clear();
}

void test_runtime_event_strings()
{
    require(to_string(runtime_event_type::publisher_connected) == "publisher_connected", "runtime event type string");
    require(to_string(runtime_direction::output) == "output", "runtime event direction string");
    require(to_string(runtime_protocol::whep) == "whep", "runtime event protocol string");
    require(to_string(runtime_state::streaming) == "streaming", "runtime event state string");
    require(to_string(runtime_end_reason::server_shutdown) == "server_shutdown", "runtime event end reason string");
    require(terminal_event_type(runtime_event_type::source_stopped, runtime_end_reason::protocol_error) == runtime_event_type::protocol_error,
            "runtime event protocol terminal type");
    require(terminal_event_type(runtime_event_type::output_stopped, runtime_end_reason::timeout) == runtime_event_type::runtime_error,
            "runtime event timeout terminal type");
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
