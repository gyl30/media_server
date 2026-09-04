#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>

#include <boost/asio/buffer.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/udp_yield_transport.h"

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

static_assert(std::is_constructible_v<udp_yield_transport, boost::asio::io_context&>);
static_assert(!has_socket_accessor<udp_yield_transport>);

void test_startup_requires_concrete_address()
{
    boost::asio::io_context io;
    udp_yield_transport transport(io);
    boost::system::error_code error;
    transport.startup(boost::asio::ip::address_v4::any(), 0, error);
    require(error == boost::asio::error::invalid_argument, "udp yield transport rejects unspecified address");
}

void test_read_write()
{
    boost::asio::io_context io;
    udp_yield_transport transport(io);
    boost::system::error_code error;
    transport.startup(boost::asio::ip::address_v4::loopback(), 0, error);
    require(!error, "udp yield transport startup");

    const auto local = transport.local_endpoint(error);
    require(!error && local.address() == boost::asio::ip::address_v4::loopback() && local.port() != 0, "udp yield transport local endpoint");

    boost::asio::ip::udp::socket peer(io, {boost::asio::ip::address_v4::loopback(), 0});
    const std::array<std::uint8_t, 4> inbound{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> outbound{5, 6, 7};
    std::array<std::uint8_t, 4> received{};
    require(peer.send_to(boost::asio::buffer(inbound), local) == inbound.size(), "udp yield transport peer send");

    bool completed = false;
    boost::asio::spawn(io,
                       [&](boost::asio::yield_context yield)
                       {
                           boost::asio::ip::udp::endpoint remote;
                           boost::system::error_code read_error;
                           const auto bytes = transport.read(received, remote, yield, read_error);
                           require(!read_error && bytes == inbound.size() && received == inbound, "udp yield transport read");
                           require(remote == peer.local_endpoint(), "udp yield transport read endpoint");

                           boost::system::error_code write_error;
                           const auto written = transport.write(outbound, remote, yield, write_error);
                           require(!write_error && written == outbound.size(), "udp yield transport write");
                           completed = true;
                       },
                       boost::asio::detached);
    io.run();
    require(completed, "udp yield transport coroutine completed");

    std::array<std::uint8_t, 3> reply{};
    boost::asio::ip::udp::endpoint sender;
    require(peer.receive_from(boost::asio::buffer(reply), sender) == outbound.size(), "udp yield transport peer receive");
    require(reply == outbound && sender == local, "udp yield transport peer received write");
    transport.shutdown();
}

void test_connect_filters_peer()
{
    boost::asio::io_context io;
    udp_yield_transport transport(io);
    boost::system::error_code error;
    transport.startup(boost::asio::ip::address_v4::loopback(), 0, error);
    require(!error, "udp yield transport connected startup");
    const auto local = transport.local_endpoint(error);
    require(!error, "udp yield transport connected local endpoint");

    boost::asio::ip::udp::socket first(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket second(io, {boost::asio::ip::address_v4::loopback(), 0});
    transport.connect(first.local_endpoint(), error);
    require(!error, "udp yield transport connect");

    const std::array<std::uint8_t, 1> rejected{9};
    const std::array<std::uint8_t, 1> accepted{7};
    second.send_to(boost::asio::buffer(rejected), local);
    first.send_to(boost::asio::buffer(accepted), local);

    std::array<std::uint8_t, 1> received{};
    boost::asio::ip::udp::endpoint remote;
    bool completed = false;
    boost::asio::spawn(io,
                       [&](boost::asio::yield_context yield)
                       {
                           boost::system::error_code read_error;
                           const auto bytes = transport.read(received, remote, yield, read_error);
                           require(!read_error && bytes == accepted.size() && received == accepted, "udp yield transport connected read");
                           completed = true;
                       },
                       boost::asio::detached);
    io.run();
    require(completed && remote == first.local_endpoint(), "udp yield transport connected peer filter");
    transport.shutdown();
}

void test_shutdown_cancels_read()
{
    boost::asio::io_context io;
    udp_yield_transport transport(io);
    boost::system::error_code error;
    transport.startup(boost::asio::ip::address_v4::loopback(), 0, error);
    require(!error, "udp yield transport shutdown startup");

    std::array<std::uint8_t, 1> data{};
    boost::asio::ip::udp::endpoint remote;
    boost::system::error_code read_error;
    bool completed = false;
    boost::asio::spawn(io,
                       [&](boost::asio::yield_context yield)
                       {
                           static_cast<void>(transport.read(data, remote, yield, read_error));
                           completed = true;
                       },
                       boost::asio::detached);

    require(io.run_one() == 1, "udp yield transport pending read starts");
    transport.shutdown();
    io.run();
    require(completed && static_cast<bool>(read_error), "udp yield transport shutdown cancels read");
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_startup_requires_concrete_address();
    std::cout << "[pass] udp_yield_transport_startup_requires_concrete_address\n";
    media_server::test_read_write();
    std::cout << "[pass] udp_yield_transport_read_write\n";
    media_server::test_connect_filters_peer();
    std::cout << "[pass] udp_yield_transport_connect_filters_peer\n";
    media_server::test_shutdown_cancels_read();
    std::cout << "[pass] udp_yield_transport_shutdown_cancels_read\n";
    return 0;
}
