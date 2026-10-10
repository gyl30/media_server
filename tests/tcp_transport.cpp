#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>

#include "media/net/tcp_transport.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
constexpr std::size_t high_water_mark = 1024U * 1024U;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct connection
{
    boost::asio::io_context io;
    tcp::socket peer{io};
    std::shared_ptr<tcp_transport> transport;

    explicit connection(bool small_receive_buffer = false)
    {
        const auto loopback = boost::asio::ip::make_address("127.0.0.1");
        tcp::acceptor listener(io, {loopback, 0});
        peer.open(tcp::v4());
        if (small_receive_buffer)
        {
            peer.set_option(tcp::socket::receive_buffer_size(4096));
        }
        peer.connect(listener.local_endpoint());
        tcp::socket socket(io);
        listener.accept(socket);
        socket.set_option(tcp::socket::send_buffer_size(4096));
        socket.set_option(tcp::no_delay(true));
        transport = std::make_shared<tcp_transport>(std::move(socket));
    }

    void drain()
    {
        io.restart();
        io.run_for(5s);
        require(io.stopped(), "TCP operation missed deadline");
    }

    void read_to_end(std::vector<std::uint8_t>& received)
    {
        boost::asio::spawn(io, [&](boost::asio::yield_context yield)
                          {
                              std::array<std::uint8_t, 8192> buffer{};
                              for (;;)
                              {
                                  boost::system::error_code error;
                                  const auto bytes = peer.async_read_some(boost::asio::buffer(buffer), yield[error]);
                                  received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
                                  if (error == boost::asio::error::eof)
                                  {
                                      return;
                                  }
                                  require(!error, "peer read failed");
                              }
                          }, [](std::exception_ptr error)
                          {
                              if (error)
                              {
                                  std::rethrow_exception(error);
                              }
                          });
    }
};

void serialization_and_ownership()
{
    connection pair;
    boost::system::error_code error;
    require(pair.transport->local_endpoint(error) == pair.peer.remote_endpoint() && !error, "local endpoint changed");
    require(pair.transport->remote_endpoint(error) == pair.peer.local_endpoint() && !error, "remote endpoint changed");
    std::vector<std::uint8_t> received;
    std::vector<std::uint8_t> expected;
    std::size_t completions{};
    pair.read_to_end(received);
    pair.transport->set_write_callback([&](boost::system::error_code result, std::size_t bytes)
                                      {
                                          require(!result && bytes == (completions < 20 ? 32'768U + completions : 257U),
                                                  "completion order/size corrupted");
                                          ++completions;
                                          if (completions == 20)
                                          {
                                              pair.transport->write(std::vector<std::uint8_t>(257, 20));
                                          }
                                          if (completions == 21)
                                          {
                                              pair.transport->shutdown();
                                          }
                                      });
    pair.transport->write(std::vector<std::uint8_t>{});
    pair.transport->write(std::span<const std::uint8_t>{});
    for (std::uint8_t sequence = 0; sequence < 20; ++sequence)
    {
        std::vector<std::uint8_t> packet(32'768U + sequence, sequence);
        expected.insert(expected.end(), packet.begin(), packet.end());
        if (sequence % 2 == 0)
        {
            pair.transport->write(std::span<const std::uint8_t>{packet});
            std::fill(packet.begin(), packet.end(), 0xff);
        }
        else
        {
            pair.transport->write(std::move(packet));
        }
    }
    expected.insert(expected.end(), 257, 20);
    pair.drain();
    require(completions == 21 && received == expected, "TCP writes missing/interleaved or buffer ownership broken");
}

void callback_shutdown()
{
    connection pair;
    std::vector<std::uint8_t> received;
    std::size_t completions{};
    auto callback_owner = std::make_shared<int>(0);
    const std::weak_ptr<int> callback_lifetime = callback_owner;
    const std::weak_ptr<tcp_transport> lifetime = pair.transport;
    pair.read_to_end(received);
    pair.transport->set_write_callback([&, callback_owner](boost::system::error_code error, std::size_t bytes)
                                      {
                                          require(!error && bytes == 73, "callback shutdown write failed");
                                          ++completions;
                                          pair.transport->shutdown();
                                          pair.transport->shutdown();
                                          pair.transport->write(std::vector<std::uint8_t>(91, 0xff));
                                          pair.transport.reset();
                                      });
    callback_owner.reset();
    pair.transport->write(std::vector<std::uint8_t>(73, 0x11));
    pair.drain();
    require(completions == 1 && received == std::vector<std::uint8_t>(73, 0x11), "callback shutdown admitted a late write");
    require(callback_lifetime.expired() && lifetime.expired(), "callback shutdown retained ownership");
}

void in_flight_shutdown()
{
    connection pair(true);
    auto retained = pair.transport;
    std::size_t completions{};
    bool barrier{};
    auto callback_owner = std::make_shared<int>(0);
    const std::weak_ptr<int> callback_lifetime = callback_owner;
    const std::weak_ptr<tcp_transport> lifetime = pair.transport;
    pair.transport->set_write_callback([&, callback_owner](boost::system::error_code, std::size_t) { ++completions; });
    callback_owner.reset();
    pair.transport->write(std::vector<std::uint8_t>(4U * high_water_mark, 0x11));
    pair.transport->write(std::vector<std::uint8_t>(101, 0x22));
    pair.transport->write(std::vector<std::uint8_t>(103, 0x33));
    std::array<std::uint8_t, 1> prefix{};
    boost::asio::async_read(pair.peer, boost::asio::buffer(prefix), [&](boost::system::error_code error, std::size_t bytes)
                            {
                                require(!error && bytes == 1 && prefix[0] == 0x11, "in-flight write never started");
                                require(completions == 0 && !callback_lifetime.expired(), "blocked write already completed");
                                pair.transport->shutdown();
                                pair.transport->shutdown();
                                pair.transport->write(std::vector<std::uint8_t>(107, 0x44));
                                pair.transport.reset();
                                boost::asio::post(pair.io, [&]()
                                                  {
                                                      require(callback_lifetime.expired(), "shutdown retained callback");
                                                      require(completions == 0, "cancelled completion escaped shutdown fence");
                                                      barrier = true;
                                                  });
                            });
    pair.drain();
    require(barrier && completions == 0, "in-flight shutdown notified callback or missed barrier");
    retained.reset();
    require(lifetime.expired(), "in-flight shutdown retained transport");
    std::vector<std::uint8_t> received;
    pair.read_to_end(received);
    pair.drain();
    require(received.size() + 1U < 4U * high_water_mark, "blocked write completed before shutdown");
    for (const auto byte : received)
    {
        require(byte == 0x11, "pending write started before shutdown");
    }
}

void high_water_boundaries()
{
    for (const auto remaining : std::array{high_water_mark, high_water_mark + 1U, std::size_t{0}})
    {
        connection pair;
        const bool overflow = remaining > high_water_mark;
        const std::size_t front_bytes = remaining == 0 ? high_water_mark + 1U : 17U;
        std::size_t completions{};
        std::vector<std::uint8_t> received;
        pair.read_to_end(received);
        pair.transport->set_write_callback([&](boost::system::error_code error, std::size_t bytes)
                                          {
                                              // 网络层只报告溢出，连接由上层关闭。
                                              if (overflow)
                                              {
                                                  require(error == boost::asio::error::no_buffer_space && bytes == 0, "high water overflow not rejected");
                                                  boost::system::error_code endpoint_error;
                                                  const auto endpoint = pair.transport->local_endpoint(endpoint_error);
                                                  require(!endpoint_error && endpoint.port() != 0, "transport closed socket before reporting overflow");
                                                  boost::asio::post(pair.io, [&]() { pair.transport->shutdown(); });
                                              }
                                              else
                                              {
                                                  require(!error && bytes == (completions == 0 ? front_bytes : remaining), "high water completion bytes changed");
                                              }
                                              ++completions;
                                              if (!overflow && (remaining == 0 || completions == 2))
                                              {
                                                  pair.transport->shutdown();
                                              }
                                          });
        pair.transport->write(std::vector<std::uint8_t>(front_bytes, 0x11));
        pair.transport->write(std::vector<std::uint8_t>(remaining, 0x22));
        if (overflow)
        {
            pair.transport->write(std::vector<std::uint8_t>(19, 0x33));
            pair.transport->write(std::vector<std::uint8_t>(23, 0x44));
        }
        pair.drain();
        std::vector<std::uint8_t> expected(front_bytes, 0x11);
        if (overflow)
        {
            // 在途的首个写可能已发出部分数据，超限数据绝不能发出。
            require(received.size() <= front_bytes && std::ranges::all_of(received, [](std::uint8_t byte) { return byte == 0x11; }) &&
                        completions == 1U,
                    "high water overflow sent rejected data or notified twice");
        }
        else
        {
            expected.insert(expected.end(), remaining, 0x22);
            require(received == expected && completions == (remaining != 0 ? 2U : 1U), "high water discarded accepted data");
        }
        pair.transport->shutdown();
        pair.transport.reset();
        pair.drain();
    }
}

void stalled_reader()
{
    connection pair(true);
    std::size_t overflows{};
    std::size_t completed_bytes{};
    pair.transport->set_write_callback([&](boost::system::error_code error, std::size_t bytes)
                                      {
                                          if (error == boost::asio::error::no_buffer_space)
                                          {
                                              ++overflows;
                                              pair.transport->shutdown();
                                              return;
                                          }
                                          require(!error, "stalled reader write failed");
                                          completed_bytes += bytes;
                                      });
    for (int index = 0; index < 64; ++index)
    {
        pair.transport->write(std::vector<std::uint8_t>(64U * 1024U, 0x33));
    }
    pair.drain();
    require(overflows == 1, "stalled reader did not hit high water once");
    require(completed_bytes < 2U * high_water_mark, "stalled reader accepted unbounded data");
    pair.transport.reset();
}

void socket_error()
{
    connection pair;
    std::size_t errors{};
    bool reset_received{};
    const std::weak_ptr<tcp_transport> lifetime = pair.transport;
    pair.transport->set_write_callback([&](boost::system::error_code error, std::size_t bytes)
                                      {
                                          require((error == boost::asio::error::broken_pipe || error == boost::asio::error::connection_reset) && bytes == 0,
                                                  "real TCP write error not propagated");
                                          ++errors;
                                      });
    boost::asio::spawn(pair.io, [&](boost::asio::yield_context yield)
                      {
                          std::array<std::uint8_t, 1> buffer{};
                          boost::system::error_code error;
                          pair.transport->read(buffer, yield, error);
                          require(error == boost::asio::error::connection_reset, "peer reset not observed");
                          reset_received = true;
                          pair.transport->write(std::vector<std::uint8_t>(4096, 0x11));
                          pair.transport->write(std::vector<std::uint8_t>(4096, 0x22));
                      }, [](std::exception_ptr error)
                      {
                          if (error)
                          {
                              std::rethrow_exception(error);
                          }
                      });
    pair.peer.set_option(tcp::socket::linger(true, 0));
    pair.peer.close();
    pair.drain();
    require(reset_received && errors == 1, "write error continued pending queue or missed callback");
    pair.transport->shutdown();
    pair.transport.reset();
    pair.drain();
    require(lifetime.expired() && errors == 1, "socket error cleanup retained transport or notified twice");
}
}    // namespace

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "expected TCP test case");
        const std::string_view name{argv[1]};
        if (name == "serialization")
        {
            serialization_and_ownership();
        }
        else if (name == "callback_shutdown")
        {
            callback_shutdown();
        }
        else if (name == "in_flight_shutdown")
        {
            in_flight_shutdown();
        }
        else if (name == "high_water")
        {
            high_water_boundaries();
        }
        else if (name == "stalled_reader")
        {
            stalled_reader();
        }
        else if (name == "socket_error")
        {
            socket_error();
        }
        else
        {
            throw std::runtime_error("unknown TCP test case");
        }
        std::cout << "TCP " << name << ": PASS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
