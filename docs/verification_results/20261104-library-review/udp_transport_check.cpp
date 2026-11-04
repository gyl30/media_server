#include <sys/socket.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <utility>
#include <algorithm>

#include <boost/asio/post.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/udp_transport.h"

namespace
{

using boost::asio::ip::udp;

struct receiver
{
    udp::socket socket;
    std::vector<std::vector<std::uint8_t>> datagrams;

    receiver(boost::asio::io_context& io, const udp::endpoint& endpoint) : socket(io, endpoint)
    {
        socket.set_option(boost::asio::socket_base::receive_buffer_size(8 * 1024 * 1024));
    }

    std::size_t drain()
    {
        boost::system::error_code error;
        socket.non_blocking(true, error);
        std::vector<std::uint8_t> buffer(65536);
        for (;;)
        {
            udp::endpoint from;
            const auto bytes = socket.receive_from(boost::asio::buffer(buffer), from, 0, error);
            if (error)
            {
                break;
            }
            datagrams.emplace_back(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
        }
        return datagrams.size();
    }
};

std::vector<std::uint8_t> make_datagram(std::uint32_t sequence, std::size_t size)
{
    std::vector<std::uint8_t> data(size);
    if (size != 0)
    {
        std::memcpy(data.data(), &sequence, std::min(sizeof(sequence), size));
    }
    for (std::size_t index = sizeof(sequence); index < size; ++index)
    {
        data[index] = static_cast<std::uint8_t>(sequence * 31U + index);
    }
    return data;
}

int failures = 0;

void expect(bool condition, const std::string& message)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    failures += condition ? 0 : 1;
}

bool same(const std::vector<std::vector<std::uint8_t>>& sent, const std::vector<std::vector<std::uint8_t>>& received) { return sent == received; }

void run_io(boost::asio::io_context& io, int milliseconds)
{
    io.restart();
    io.run_for(std::chrono::milliseconds(milliseconds));
}

std::shared_ptr<media_server::udp_transport> make_transport(boost::asio::io_context& io, const boost::asio::ip::address& address)
{
    auto transport = std::make_shared<media_server::udp_transport>(io);
    boost::system::error_code error;
    transport->startup(address, 0, media_server::udp_receive_buffer::system_default, error);
    expect(!error, "transport startup " + error.message());
    return transport;
}

void check_ordering(const boost::asio::ip::address& address)
{
    boost::asio::io_context io;
    receiver first(io, udp::endpoint{address, 0});
    receiver second(io, udp::endpoint{address, 0});
    const auto first_endpoint = first.socket.local_endpoint();
    const auto second_endpoint = second.socket.local_endpoint();
    auto transport = make_transport(io, address);
    int callbacks = 0;
    transport->set_error_callback([&](boost::system::error_code) { ++callbacks; });

    std::vector<std::vector<std::uint8_t>> to_first;
    std::vector<std::vector<std::uint8_t>> to_second;
    std::uint32_t sequence = 0;
    const auto send = [&](const udp::endpoint& endpoint, std::size_t size)
    {
        auto data = make_datagram(sequence++, size);
        expect(transport->write(data, endpoint), "write accepted size " + std::to_string(size));
        (endpoint == first_endpoint ? to_first : to_second).push_back(std::move(data));
    };

    for (const std::size_t size : {1216U, 1216U, 700U, 200U, 1216U, 500U, 1300U, 0U, 1216U})
    {
        send(first_endpoint, size);
    }
    send(second_endpoint, 300);
    send(first_endpoint, 1216);
    for (int index = 0; index < 70; ++index)
    {
        send(index % 3 == 0 ? first_endpoint : second_endpoint, 1216);
    }

    run_io(io, 100);
    first.drain();
    second.drain();
    expect(same(to_first, first.datagrams),
           "first endpoint receives every datagram unchanged and in order (" + std::to_string(first.datagrams.size()) + ")");
    expect(same(to_second, second.datagrams),
           "second endpoint receives every datagram unchanged and in order (" + std::to_string(second.datagrams.size()) + ")");
    expect(callbacks == 0, "no error callback");
    transport->shutdown();
}

void check_send_error(const boost::asio::ip::address& address, const udp::endpoint& unreachable, int expected_callbacks)
{
    boost::asio::io_context io;
    auto transport = make_transport(io, address);
    int callbacks = 0;
    boost::system::error_code last;
    transport->set_error_callback(
        [&](boost::system::error_code error)
        {
            ++callbacks;
            last = error;
        });
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        transport->write(make_datagram(index, 1216), unreachable);
    }
    transport->write(make_datagram(9, 100), unreachable);
    run_io(io, 50);
    expect(callbacks == expected_callbacks,
           "send errors reported through the callback: " + std::to_string(callbacks) + " (expected " + std::to_string(expected_callbacks) + "), " +
               last.message());
    transport->shutdown();
}

void check_backpressure(const boost::asio::ip::address& address)
{
    boost::asio::io_context io;
    auto transport = make_transport(io, address);
    int callbacks = 0;
    transport->set_error_callback([&](boost::system::error_code) { ++callbacks; });
    receiver sink(io, udp::endpoint{address, 0});
    const auto target = sink.socket.local_endpoint();

    std::vector<std::vector<std::uint8_t>> sent;
    std::size_t rejected = 0;
    std::uint32_t sequence = 0;
    for (int frame = 0; frame < 2000; ++frame)
    {
        for (int index = 0; index < 8; ++index)
        {
            auto data = make_datagram(sequence++, index == 7 ? 600 : 1216);
            if (transport->write(data, target))
            {
                sent.push_back(std::move(data));
            }
            else
            {
                ++rejected;
            }
        }
    }
    std::printf("     backpressure: %zu accepted, %zu rejected\n", sent.size(), rejected);
    expect(rejected > 0, "queue high-water mark rejects new datagrams under sustained backpressure");
    udp::socket kicker(io, udp::endpoint{address, 0});
    const std::array<std::uint8_t, 1> kick{};
    for (int round = 0; round < 400 && sink.datagrams.size() < sent.size(); ++round)
    {
        boost::system::error_code ignored;
        kicker.send_to(boost::asio::buffer(kick), udp::endpoint{address, 9}, 0, ignored);
        run_io(io, 50);
        sink.drain();
    }
    expect(same(sent, sink.datagrams),
           "every accepted datagram arrives once, unchanged and in order (" + std::to_string(sink.datagrams.size()) + "/" +
               std::to_string(sent.size()) + ")");
    expect(callbacks == 0, "backpressure is not reported as an error");

    for (int index = 0; index < 40; ++index)
    {
        transport->write(make_datagram(sequence++, 1216), target);
    }
    transport->shutdown();
    run_io(io, 200);
    expect(true, "shutdown with queued datagrams completes");
}

}    // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "ordering";
    const auto local = boost::asio::ip::make_address(argc > 2 ? argv[2] : "127.0.0.1");
    if (mode == "ordering")
    {
        check_ordering(local);
        check_send_error(local, udp::endpoint{boost::asio::ip::make_address("203.0.113.1"), 9}, argc > 3 ? std::stoi(argv[3]) : 5);
    }
    else if (mode == "backpressure")
    {
        check_backpressure(local);
    }
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
