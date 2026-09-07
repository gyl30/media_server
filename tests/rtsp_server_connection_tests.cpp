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

#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace
{

using namespace std::chrono_literals;
using boost::asio::ip::tcp;
using media_server::video_transcode_codec;
using media_server::rtsp_server_connection;
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

std::string send_options(tcp::socket& socket, int cseq)
{
    boost::system::error_code error;
    socket.non_blocking(false, error);
    if (error)
    {
        return {};
    }

    const auto request =
        "OPTIONS rtsp://127.0.0.1/live/test RTSP/1.0\r\nCSeq: " + std::to_string(cseq) + "\r\nContent-Length: 0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), error);
    if (error)
    {
        return {};
    }
    return read_headers(socket, 500ms);
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

}    // namespace

int main()
{
    try
    {
        test_idle_connection_timeout();
        std::cout << "[pass] rtsp_server_connection_idle_timeout\n";
        test_control_activity_refreshes_timeout();
        std::cout << "[pass] rtsp_server_connection_control_refreshes_timeout\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
