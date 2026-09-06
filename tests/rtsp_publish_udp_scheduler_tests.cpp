#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/ip/address.hpp>

#include "media/core/stream_registry.h"
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_publish_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{
namespace
{

using namespace std::chrono_literals;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

struct server_fixture
{
    rtsp_publish_session* publish{};
    std::string response;
};

int close_callback(void*) { return 0; }

int send_callback(void* param, const void* data, std::size_t bytes)
{
    auto& fixture = *static_cast<server_fixture*>(param);
    fixture.response.append(static_cast<const char*>(data), bytes);
    return 0;
}

int announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length)
{
    return static_cast<server_fixture*>(param)->publish->on_announce(server, uri, sdp, length);
}

int setup_callback(void* param,
                   rtsp_server_t* server,
                   const char* uri,
                   const char* session,
                   const rtsp_header_transport_t transports[],
                   std::size_t count)
{
    return static_cast<server_fixture*>(param)->publish->on_setup(
        server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int record_callback(void* param,
                    rtsp_server_t* server,
                    const char* uri,
                    const char* session,
                    const std::int64_t* npt,
                    const double* scale)
{
    return static_cast<server_fixture*>(param)->publish->on_record(server, uri, session, npt, scale);
}

std::string input_request(rtsp_server_t* server, server_fixture& fixture, const std::string& request)
{
    fixture.response.clear();
    auto bytes = request.size();
    require(rtsp_server_input(server, request.data(), &bytes) == 0 && bytes == 0, "rtsp publish udp request input");
    return fixture.response;
}

std::string session_id(const std::string& response)
{
    const auto header = response.find("Session:");
    require(header != std::string::npos, "rtsp publish udp SETUP session header");
    auto begin = header + std::string_view{"Session:"}.size();
    while (begin < response.size() && response[begin] == ' ')
    {
        ++begin;
    }
    const auto line_end = response.find("\r\n", begin);
    require(line_end != std::string::npos && line_end > begin, "rtsp publish udp SETUP session value");
    const auto parameter = response.find(';', begin);
    const auto end = parameter != std::string::npos && parameter < line_end ? parameter : line_end;
    return response.substr(begin, end - begin);
}

void test_rtcp_scheduler_releases_after_shutdown()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();

    rtsp_publish_session publish(worker,
                                 boost::asio::ip::address_v4::loopback(),
                                 [](std::span<const std::uint8_t>) {},
                                 0ms);
    publish.set_error_handler([](boost::system::error_code) {});

    server_fixture fixture{.publish = &publish, .response = {}};
    rtsp_handler_t handler{};
    handler.close = &close_callback;
    handler.send = &send_callback;
    handler.onannounce = &announce_callback;
    handler.onsetup = &setup_callback;
    handler.onrecord = &record_callback;

    auto* server = rtsp_server_create("127.0.0.1", 8554, &handler, &fixture, &fixture);
    require(server != nullptr, "rtsp publish udp server create");

    const std::string uri = "rtsp://127.0.0.1/live/rtcp-scheduler";
    const std::string track_uri = uri + "/trackID=0";
    const auto sdp =
        std::string("v=0\r\n") +
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=media_server\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;"
        "sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
        "a=control:" + track_uri + "\r\n";

    const auto announce =
        "ANNOUNCE " + uri + " RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    require(input_request(server, fixture, announce).starts_with("RTSP/1.0 200"), "rtsp publish udp ANNOUNCE");

    const auto setup =
        "SETUP " + track_uri + " RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Transport: RTP/AVP;unicast;client_port=40000-40001;mode=record\r\n\r\n";
    const auto setup_response = input_request(server, fixture, setup);
    require(setup_response.starts_with("RTSP/1.0 200"), "rtsp publish udp SETUP");
    const auto session = session_id(setup_response);

    const auto record =
        "RECORD " + uri + " RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Session: " + session + "\r\n\r\n";
    require(input_request(server, fixture, record).starts_with("RTSP/1.0 200"), "rtsp publish udp RECORD");

    publish.shutdown();
    require(rtsp_server_destroy(server) == 0, "rtsp publish udp server destroy");

    io.run_for(100ms);
    require(io.stopped(), "rtsp publish udp RTCP scheduler released after shutdown");
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::port_manager::init(33'000, 33'099);
    media_server::registry::init();
    try
    {
        media_server::test_rtcp_scheduler_releases_after_shutdown();
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cout << "[pass] rtsp_publish_udp_rtcp_scheduler_shutdown\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cerr << "[fail] rtsp_publish_udp_rtcp_scheduler_shutdown: " << error.what() << '\n';
        return 1;
    }
}
