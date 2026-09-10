#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace
{

using namespace std::chrono_literals;
using boost::asio::ip::tcp;
using media_server::port_manager;
using media_server::rtsp_server_connection;
using media_server::video_transcode_codec;
using media_server::worker_context;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

bool wait_for_close(tcp::socket& socket, std::chrono::milliseconds timeout)
{
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error)
    {
        return false;
    }

    std::array<std::uint8_t, 1> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        error.clear();
        const auto bytes = socket.read_some(boost::asio::buffer(buffer), error);
        if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset)
        {
            return true;
        }
        if (!error && bytes != 0)
        {
            return false;
        }
        if (error && error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

std::string read_headers(tcp::socket& socket, std::chrono::milliseconds timeout)
{
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error)
    {
        return {};
    }

    std::string response;
    std::array<char, 1024> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        error.clear();
        const auto bytes = socket.read_some(boost::asio::buffer(buffer), error);
        if (!error)
        {
            response.append(buffer.data(), bytes);
            if (response.find("\r\n\r\n") != std::string::npos)
            {
                return response;
            }
            continue;
        }
        if (error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        {
            return {};
        }
        std::this_thread::sleep_for(5ms);
    }
    return {};
}

std::string send_request(tcp::socket& socket, const std::string& request)
{
    boost::system::error_code error;
    socket.non_blocking(false, error);
    if (error)
    {
        return {};
    }

    boost::asio::write(socket, boost::asio::buffer(request), error);
    if (error)
    {
        return {};
    }
    return read_headers(socket, 500ms);
}

std::string send_options(tcp::socket& socket, int cseq)
{
    const auto request =
        "OPTIONS rtsp://127.0.0.1/live/test RTSP/1.0\r\nCSeq: " + std::to_string(cseq) + "\r\nContent-Length: 0\r\n\r\n";
    return send_request(socket, request);
}

std::string publish_announce(std::string_view uri)
{
    const auto track_uri = std::string(uri) + "/trackID=0";
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

    return "ANNOUNCE " + std::string(uri) +
           " RTSP/1.0\r\nCSeq: 1\r\nContent-Type: application/sdp\r\nContent-Length: " +
           std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
}

void test_idle_connection_timeout()
{
    worker_context worker;
    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto connection =
        std::make_shared<rtsp_server_connection>(worker, std::move(server_socket), video_transcode_codec::passthrough, 100ms);
    connection->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    const bool closed = wait_for_close(client, 500ms);
    connection->shutdown();
    runner.join();
    require(closed, "idle RTSP server connection timeout");
}

void test_control_activity_refreshes_timeout()
{
    worker_context worker;
    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto connection =
        std::make_shared<rtsp_server_connection>(worker, std::move(server_socket), video_transcode_codec::passthrough, 200ms);
    connection->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    std::this_thread::sleep_for(120ms);
    const auto first = send_options(client, 1);
    std::this_thread::sleep_for(120ms);
    const auto second = send_options(client, 2);
    const bool closed = wait_for_close(client, 500ms);

    connection->shutdown();
    runner.join();

    require(first.starts_with("RTSP/1.0 200"), "first OPTIONS response");
    require(second.starts_with("RTSP/1.0 200"), "OPTIONS refreshes RTSP inactivity timeout");
    require(closed, "refreshed RTSP server connection eventually times out");
}

void test_udp_setup_internal_failure_closes_connection()
{
    port_manager::init(33'000, 33'001);
    const auto reserved = port_manager::instance().acquire_pair();
    require(reserved.has_value(), "reserve RTSP UDP media ports");

    worker_context worker;
    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto connection =
        std::make_shared<rtsp_server_connection>(worker, std::move(server_socket), video_transcode_codec::passthrough, 5s);
    connection->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    const std::string uri = "rtsp://127.0.0.1/live/udp-internal-failure";
    const auto announce_response = send_request(client, publish_announce(uri));
    const auto setup_request =
        "SETUP " + uri +
        "/trackID=0 RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP;unicast;client_port=40000-40001;mode=record\r\n\r\n";
    const auto setup_response = send_request(client, setup_request);
    const bool closed = wait_for_close(client, 500ms);

    connection->shutdown();
    runner.join();
    port_manager::instance().release(*reserved);
    port_manager::destroy();

    require(announce_response.starts_with("RTSP/1.0 200"), "UDP internal failure ANNOUNCE response");
    require(setup_response.empty(), "UDP internal setup failure must not send RTSP response");
    require(closed, "UDP internal setup failure closes RTSP connection");
}

void test_record_internal_failure_closes_connection()
{
    worker_context worker;
    auto existing = std::make_shared<media_server::media_stream>("live/record-internal-failure", worker);
    require(existing->set_tracks({media_server::media_track{
                .id = 1,
                .kind = media_server::media_kind::video,
                .codec = media_server::codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = {0x00, 0x00, 0x00, 0x01, 0x67},
            }}),
            "record internal failure existing stream tracks");
    require(media_server::stream_registry::instance().add(existing), "record internal failure existing stream registry");

    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto connection =
        std::make_shared<rtsp_server_connection>(worker, std::move(server_socket), video_transcode_codec::passthrough, 5s);
    connection->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    const std::string uri = "rtsp://127.0.0.1/live/record-internal-failure";
    const auto announce_response = send_request(client, publish_announce(uri));
    const auto setup_request =
        "SETUP " + uri +
        "/trackID=0 RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n";
    const auto setup_response = send_request(client, setup_request);

    std::string session;
    const auto header = setup_response.find("Session:");
    if (header != std::string::npos)
    {
        auto begin = header + std::string_view{"Session:"}.size();
        while (begin < setup_response.size() && setup_response[begin] == ' ')
        {
            ++begin;
        }
        const auto line_end = setup_response.find("\r\n", begin);
        if (line_end != std::string::npos && line_end > begin)
        {
            const auto parameter = setup_response.find(';', begin);
            const auto end = parameter != std::string::npos && parameter < line_end ? parameter : line_end;
            session = setup_response.substr(begin, end - begin);
        }
    }

    const auto record_request =
        "RECORD " + uri + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + session + "\r\n\r\n";
    const auto record_response = send_request(client, record_request);
    const bool closed = wait_for_close(client, 500ms);

    connection->shutdown();
    runner.join();
    media_server::stream_registry::instance().clear();

    require(announce_response.starts_with("RTSP/1.0 200"), "record internal failure ANNOUNCE response");
    require(setup_response.starts_with("RTSP/1.0 200"), "record internal failure SETUP response");
    require(!session.empty(), "record internal failure SETUP session");
    require(record_response.empty(), "record internal failure must not send RTSP response");
    require(closed, "record internal failure closes RTSP connection");
}

}    // namespace

int main()
{
    try
    {
        test_idle_connection_timeout();
        std::cout << "[pass] rtsp_server_connection_idle_timeout\n";
        test_control_activity_refreshes_timeout();
        std::cout << "[pass] rtsp_server_connection_control_refreshes_timeout\n";
        test_udp_setup_internal_failure_closes_connection();
        std::cout << "[pass] rtsp_server_connection_udp_setup_internal_failure\n";
        test_record_internal_failure_closes_connection();
        std::cout << "[pass] rtsp_server_connection_record_internal_failure\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
