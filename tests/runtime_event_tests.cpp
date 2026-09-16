#include <chrono>
#include <memory>
#include <string>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/post.hpp>
#include <boost/json/parse.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/http/event_reporter.h"
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
    signaling_client::instance().configure(worker.io(),
                                           {
                                               .signaling_url = std::move(url),
                                               .server_id = "media-1",
                                               .instance_id = "instance-1",
                                               .control_url = "http://127.0.0.1:8080",
                                               .media_ip = "127.0.0.1",
                                               .rtmp_port = 1935,
                                               .rtsp_port = 8554,
                                               .http_port = 8080,
                                           });
    event_reporter::instance().configure(worker.io(), "media-1", "instance-1");
}

void require_identity(const boost::json::object& event)
{
    require(event.at("server_id") == "media-1", "runtime event server id");
    require(event.at("instance_id") == "instance-1", "runtime event instance id");
    require(event.at("stream_id") == stream_id, "runtime event stream id");
    require(event.at("stream_name") == "live/runtime-events", "runtime event stream name");
    require(event.at("source_id") == source_id, "runtime event source id");
    require(event.at("kind") == "source", "runtime event kind");
    require(event.at("protocol") == "rtsp", "runtime event protocol");
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

    worker.release_work();
    worker.io().run();

    const auto requests = server.requests();
    require(requests.size() == 2U, "runtime event stopped once");
    const auto starting = boost::json::parse(requests[0].body).as_object();
    const auto stopped = boost::json::parse(requests[1].body).as_object();
    require_identity(starting);
    require(starting.at("state") == "starting", "runtime event source starting");
    require(starting.at("stage") == "resolving" && !starting.contains("end_reason") && !starting.contains("error"), "runtime event starting fields");
    require_identity(stopped);
    require(stopped.at("state") == "stopped", "runtime event source runtime failure");
    require(stopped.at("end_reason") == "runtime_error" && stopped.contains("error"), "runtime event failure fields");
    require(!stream_registry::instance().take_receiver_session("live/runtime-events"), "runtime event pull releases identity");

    session->shutdown();
    worker.io().restart();
    worker.io().run();
    require(server.requests().size() == 2U, "runtime event repeated shutdown ignored");
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

    worker.release_work();
    worker.io().run();
    const auto requests = server.requests();
    require(requests.size() == 2U, "runtime shutdown transitions once");
    const auto stopped = boost::json::parse(requests[1].body).as_object();
    require(stopped.at("state") == "stopped", "runtime shutdown terminal event");
    require(stopped.at("end_reason") == "server_shutdown" && !stopped.contains("error"), "runtime shutdown first reason wins");
    require(!stream_registry::instance().take_receiver_session("live/runtime-events"), "runtime shutdown releases identity");
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
