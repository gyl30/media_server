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
#include "media/rtmp/rtmp_session.h"

extern "C"
{
#include "rtmp-handshake.h"
}

namespace
{

using namespace std::chrono_literals;
using boost::asio::ip::tcp;
using media_server::output_video_config;
using media_server::rtmp_session;
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

void test_rtmp_write_backlog_limit()
{
    worker_context worker;
    boost::asio::io_context client_io;
    tcp::acceptor acceptor(client_io, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_socket(worker.io());
    acceptor.accept(server_socket);

    auto session =
        std::make_shared<rtmp_session>(worker, std::move(server_socket), output_video_config{}, 5s, 0U);
    session->startup();
    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    std::array<std::uint8_t, 1> c0{};
    alignas(4) std::array<std::uint8_t, RTMP_HANDSHAKE_SIZE> c1{};
    require(rtmp_handshake_c0(c0.data(), RTMP_VERSION) == 1, "RTMP C0");
    require(rtmp_handshake_c1(c1.data(), 0) == RTMP_HANDSHAKE_SIZE, "RTMP C1");

    boost::system::error_code error;
    const std::array<boost::asio::const_buffer, 2> handshake{
        boost::asio::buffer(c0),
        boost::asio::buffer(c1),
    };
    boost::asio::write(client, handshake, error);
    require(!error, "RTMP handshake write");

    const bool closed = wait_for_close(client, 500ms);
    session->shutdown();
    runner.join();
    require(closed, "RTMP write backlog limit closes connection");
}

}    // namespace

int main()
{
    try
    {
        test_rtmp_write_backlog_limit();
        std::cout << "[pass] rtmp_write_backlog_limit\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
