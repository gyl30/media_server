#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detached.hpp>

#include "media/net/tcp_yield_transport.h"

namespace media_server
{
namespace
{

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "[fail] " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

template <typename T>
concept has_socket_accessor = requires(T& transport) { transport.socket(); };

static_assert(std::is_constructible_v<tcp_yield_transport, boost::asio::ip::tcp::socket>);
static_assert(!has_socket_accessor<tcp_yield_transport>);

void test_read_write()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    tcp_yield_transport transport(acceptor.accept());

    boost::system::error_code error;
    const auto local = transport.local_endpoint(error);
    require(!error && local.port() == acceptor.local_endpoint().port(), "tcp yield transport local endpoint");
    const auto remote = transport.remote_endpoint(error);
    require(!error && remote.port() == client.local_endpoint().port(), "tcp yield transport remote endpoint");

    const std::array<std::uint8_t, 4> inbound{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> outbound{5, 6, 7};
    std::array<std::uint8_t, 4> received{};
    bool completed = false;

    boost::asio::write(client, boost::asio::buffer(inbound));
    boost::asio::spawn(io,
                       [&](boost::asio::yield_context yield)
                       {
                           boost::system::error_code read_error;
                           const auto bytes = transport.read(received, yield, read_error);
                           require(!read_error && bytes == inbound.size() && received == inbound, "tcp yield transport read");

                           boost::system::error_code write_error;
                           const auto written = transport.write(outbound, yield, write_error);
                           require(!write_error && written == outbound.size(), "tcp yield transport write");
                           completed = true;
                       },
                       boost::asio::detached);
    io.run();
    require(completed, "tcp yield transport coroutine completed");

    std::array<std::uint8_t, 3> reply{};
    boost::asio::read(client, boost::asio::buffer(reply));
    require(reply == outbound, "tcp yield transport peer received write");
    transport.shutdown();
}

void test_shutdown_cancels_read()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    tcp_yield_transport transport(acceptor.accept());

    std::array<std::uint8_t, 1> data{};
    boost::system::error_code read_error;
    bool completed = false;
    boost::asio::spawn(io,
                       [&](boost::asio::yield_context yield)
                       {
                           static_cast<void>(transport.read(data, yield, read_error));
                           completed = true;
                       },
                       boost::asio::detached);

    require(io.run_one() == 1, "tcp yield transport pending read starts");
    transport.shutdown();
    io.run();
    require(completed && static_cast<bool>(read_error), "tcp yield transport shutdown cancels read");
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_read_write();
    std::cout << "[pass] tcp_yield_transport_read_write\n";
    media_server::test_shutdown_cancels_read();
    std::cout << "[pass] tcp_yield_transport_shutdown_cancels_read\n";
    return 0;
}
