#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/asio.hpp>

#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace
{

using namespace std::chrono_literals;
using boost::asio::ip::tcp;
using media_server::output_video_codec;
using media_server::rtsp_pull_session;
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
        static_cast<void>(socket.read_some(boost::asio::buffer(buffer), error));
        if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset)
        {
            return true;
        }
        if (error && error != boost::asio::error::would_block && error != boost::asio::error::try_again)
        {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void test_rtsp_server_write_backlog_limit()
{
    worker_context worker;
    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto connection =
        std::make_shared<rtsp_server_connection>(worker, std::move(server_socket), output_video_codec::passthrough, 5s, 0U);
    connection->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    const std::string request =
        "OPTIONS rtsp://127.0.0.1/live/test RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    boost::system::error_code error;
    boost::asio::write(client, boost::asio::buffer(request), error);
    require(!error, "RTSP server backlog request write");

    const bool closed = wait_for_close(client, 500ms);
    connection->shutdown();
    runner.join();
    require(closed, "RTSP server write backlog limit closes connection");
}

void test_rtsp_pull_write_backlog_limit()
{
    boost::asio::io_context server_io;
    tcp::acceptor acceptor(server_io, {boost::asio::ip::address_v4::loopback(), 0});
    worker_context client_worker;
    const auto request_url =
        "rtsp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/live/backpressure";

    auto pull = std::make_shared<rtsp_pull_session>(
        client_worker, "relay/backpressure", request_url, 5s, 5s, 0U);
    require(pull->startup(), "RTSP pull backlog startup");
    client_worker.release_work();
    std::jthread runner([&client_worker]() { client_worker.run(); });

    tcp::socket server_socket(server_io);
    acceptor.accept(server_socket);
    const bool closed = wait_for_close(server_socket, 500ms);

    pull->shutdown();
    runner.join();
    require(closed, "RTSP pull write backlog limit closes connection");
}

}    // namespace

int main()
{
    try
    {
        test_rtsp_server_write_backlog_limit();
        std::cout << "[pass] rtsp_server_write_backlog_limit\n";
        test_rtsp_pull_write_backlog_limit();
        std::cout << "[pass] rtsp_pull_write_backlog_limit\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
