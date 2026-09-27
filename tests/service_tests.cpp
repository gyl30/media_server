#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <csignal>
#include <cstdint>
#include <utility>
#include <iostream>
#include <stdexcept>

#include <boost/asio.hpp>

#include "config.h"
#include "service.h"

namespace
{

using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::array<std::uint16_t, 3> unused_ports()
{
    boost::asio::io_context io;
    const auto address = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::tcp::acceptor first(io, {address, 0});
    boost::asio::ip::tcp::acceptor second(io, {address, 0});
    boost::asio::ip::tcp::acceptor third(io, {address, 0});
    return {first.local_endpoint().port(), second.local_endpoint().port(), third.local_endpoint().port()};
}

bool can_connect(std::uint16_t port)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), port}, error);
    return !error;
}

bool wait_listening(std::uint16_t port)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (can_connect(port))
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

media_server::config media_config()
{
    media_server::config cfg;
    cfg.threads = 3;
    const auto ports = unused_ports();
    cfg.rtmp_port = ports[0];
    cfg.rtsp_port = ports[1];
    cfg.http_port = ports[2];
    return cfg;
}

void test_media_listeners_start()
{
    auto cfg = media_config();
    const auto ports = std::array{cfg.rtmp_port, cfg.rtsp_port, cfg.http_port};
    media_server::service service(std::move(cfg));
    std::jthread runner([&]() { service.run(); });

    const bool rtmp_listening = wait_listening(ports[0]);
    const bool rtsp_listening = wait_listening(ports[1]);
    const bool http_listening = wait_listening(ports[2]);
    std::raise(SIGTERM);
    runner.join();
    require(rtmp_listening && rtsp_listening && http_listening, "media listeners start independently");
}

void test_listener_failure_stops_without_signal()
{
    auto cfg = media_config();
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor occupied(
        io, {boost::asio::ip::make_address("127.0.0.1"), cfg.rtmp_port});
    media_server::service service(std::move(cfg));
    require(service.run() == 0, "listener startup failure stops service without a signal");
}

}    // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "service test case required");
    const std::string_view test{argv[1]};
    if (test == "invalid_bind")
    {
        media_server::config cfg;
        cfg.bind_address = "0.0.0.0";
        media_server::service service(std::move(cfg));
        require(service.run() == 1, "service rejects unspecified bind address");
    }
    else if (test == "invalid_webrtc")
    {
        media_server::config cfg;
        cfg.webrtc_address = "invalid-address";
        media_server::service service(std::move(cfg));
        require(service.run() == 1, "service rejects invalid webrtc address");
    }
    else if (test == "media_listeners")
    {
        test_media_listeners_start();
    }
    else if (test == "listener_failure")
    {
        test_listener_failure_stops_without_signal();
    }
    else
    {
        throw std::runtime_error("unknown service test case");
    }

    std::cout << "[pass] service tests\n";
    return 0;
}
