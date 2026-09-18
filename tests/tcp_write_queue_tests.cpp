#include <array>
#include <memory>
#include <vector>
#include <cstdlib>
#include <utility>
#include <iostream>
#include <string_view>
#include <initializer_list>

#include <boost/asio/read.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/socket_base.hpp>

#include "media/net/tcp_write_queue.h"

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

tcp_write_queue::buffer make_buffer(std::initializer_list<std::uint8_t> bytes) { return std::make_shared<std::vector<std::uint8_t>>(bytes); }

void test_fifo_and_entry_completion()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    tcp_yield_transport transport(acceptor.accept());
    tcp_write_queue queue(4U);

    require(queue.enqueue(make_buffer({1, 2})) == tcp_write_enqueue_result::start_writer, "first entry starts writer");
    require(queue.enqueue(make_buffer({3, 4}), true) == tcp_write_enqueue_result::queued, "second entry remains queued");

    std::array<bool, 2> stop_after_write{};
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            auto result = queue.write_one(transport, yield);
            require(!result.error, "first queued write succeeds");
            stop_after_write[0] = result.stop_after_write;
            result = queue.write_one(transport, yield);
            require(!result.error, "second queued write succeeds");
            stop_after_write[1] = result.stop_after_write;
        },
        boost::asio::detached);
    io.run();

    std::array<std::uint8_t, 4> received{};
    boost::asio::read(client, boost::asio::buffer(received));
    require(received == std::array<std::uint8_t, 4>{1, 2, 3, 4}, "queue preserves FIFO bytes");
    require(!stop_after_write[0] && stop_after_write[1], "entry completion flag follows its write");
    require(queue.empty(), "successful writes drain queue");
}

void test_overflow_stops_queue_once()
{
    tcp_write_queue queue(1U);
    require(queue.enqueue(make_buffer({1, 2})) == tcp_write_enqueue_result::overflow, "first excess write reports overflow");
    require(queue.stopped(), "overflow stops queue");
    require(queue.enqueue(make_buffer({3, 4})) == tcp_write_enqueue_result::stopped, "later excess write reports stopped");
}

void test_inflight_error_survives_stop()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    auto server = acceptor.accept();
    server.set_option(boost::asio::socket_base::send_buffer_size(1024));
    tcp_yield_transport transport(std::move(server));
    tcp_write_queue queue(2U * 1024U * 1024U);
    require(queue.enqueue(std::make_shared<std::vector<std::uint8_t>>(1024U * 1024U)) == tcp_write_enqueue_result::start_writer,
            "inflight write starts writer");

    bool completed = false;
    boost::system::error_code write_error;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            const auto result = queue.write_one(transport, yield);
            write_error = result.error;
            completed = true;
        },
        boost::asio::detached);
    require(io.run_one() == 1, "inflight write coroutine starts");
    queue.stop();
    transport.shutdown();
    io.run();

    require(completed && write_error, "stopped queue returns inflight transport error");
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::test_fifo_and_entry_completion();
    std::cout << "[pass] tcp_write_queue_fifo_and_entry_completion\n";
    media_server::test_overflow_stops_queue_once();
    std::cout << "[pass] tcp_write_queue_overflow_stops_once\n";
    media_server::test_inflight_error_survives_stop();
    std::cout << "[pass] tcp_write_queue_inflight_error_survives_stop\n";
    return 0;
}
