#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/scope/scope_exit.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include "media/net/udp_transport.h"
#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using udp = boost::asio::ip::udp;
const auto loopback = boost::asio::ip::make_address("127.0.0.1");

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void rethrow(std::exception_ptr error)
{
    if (error)
    {
        std::rethrow_exception(error);
    }
}

std::shared_ptr<udp_transport> bound(boost::asio::io_context& io)
{
    auto transport = std::make_shared<udp_transport>(io);
    boost::system::error_code error;
    transport->startup(loopback, 0, error);
    require(!error, "UDP startup failed");
    return transport;
}

udp::endpoint address(const std::shared_ptr<udp_transport>& transport)
{
    boost::system::error_code error;
    const auto endpoint = transport->local_endpoint(error);
    require(!error && endpoint.port() != 0, "UDP local endpoint failed");
    return endpoint;
}

void drain(boost::asio::io_context& io)
{
    io.run_for(5s);
    require(io.stopped(), "UDP operation missed deadline");
}

void serialization_and_endpoints()
{
    boost::asio::io_context io;
    auto sender = bound(io);
    std::array receivers{bound(io), bound(io)};
    const std::array endpoints{address(receivers[0]), address(receivers[1])};
    const auto origin = address(sender);
    boost::system::error_code error;
    sender->connect(endpoints[0], error);
    require(!error, "UDP connect failed");
    std::size_t completions{};
    std::size_t received{};
    sender->set_write_callback([&](boost::system::error_code result, std::size_t bytes)
                              {
                                  require(!result && bytes == 21U + completions, "completion order/size corrupted");
                                  ++completions;
                              });
    for (std::size_t peer = 0; peer < receivers.size(); ++peer)
    {
        boost::asio::spawn(io, [&, peer](boost::asio::yield_context yield)
                          {
                              std::array<std::uint8_t, 128> buffer{};
                              udp::endpoint remote;
                              for (std::size_t sequence = peer; sequence < 40; sequence += 2)
                              {
                                  boost::system::error_code read_error;
                                  const auto bytes = receivers[peer]->read(buffer, remote, yield, read_error);
                                  require(!read_error && remote == origin, "datagram endpoint changed");
                                  require(bytes == 21U + sequence, "datagram split/merged/reordered");
                                  for (std::size_t index = 0; index < bytes; ++index)
                                  {
                                      require(buffer[index] == sequence, "datagram buffer corrupted");
                                  }
                                  ++received;
                              }
                          }, rethrow);
    }
    boost::asio::post(io, [&]()
                      {
                          for (std::size_t sequence = 0; sequence < 40; ++sequence)
                          {
                              std::vector<std::uint8_t> packet(21U + sequence, static_cast<std::uint8_t>(sequence));
                              auto endpoint = endpoints[sequence % 2];
                              if (sequence % 2 == 0)
                              {
                                  require(sender->write(std::span<const std::uint8_t>{packet}, endpoint), "span enqueue rejected");
                                  std::fill(packet.begin(), packet.end(), 0xff);
                              }
                              else
                              {
                                  require(sender->write(std::move(packet), endpoint), "vector enqueue rejected");
                              }
                              endpoint.port(0);
                          }
                      });
    drain(io);
    require(completions == 40 && received == 40, "missing datagram/completion");
}

void in_flight_shutdown()
{
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        boost::asio::io_context io;
        udp::socket receiver(io, udp::endpoint(loopback, 0));
        receiver.non_blocking(true);
        auto sender = bound(io);
        const std::weak_ptr<udp_transport> lifetime = sender;
        auto callback_owner = std::make_shared<int>(0);
        const std::weak_ptr<int> callback_lifetime = callback_owner;
        std::size_t completions{};
        sender->set_write_callback([&, callback_owner](boost::system::error_code, std::size_t) { ++completions; });
        callback_owner.reset();
        boost::asio::post(io, [&]()
                          {
                              for (std::uint8_t sequence = 0; sequence < 3; ++sequence)
                              {
                                  require(sender->write(std::vector<std::uint8_t>(60'000, sequence), receiver.local_endpoint()),
                                          "shutdown enqueue rejected");
                              }
                              // 首包已提交，completion 尚未执行；其余两包只能留在队列。
                              sender->shutdown();
                              sender->shutdown();
                              require(callback_lifetime.expired(), "shutdown retained write callback");
                              require(!sender->write(std::vector<std::uint8_t>{3}, receiver.local_endpoint()), "write after shutdown accepted");
                              sender.reset();
                              require(!lifetime.expired(), "in-flight transport released before completion");
                          });
        drain(io);
        require(lifetime.expired() && completions == 0, "shutdown completion/cycle escaped fencing");
        std::array<std::uint8_t, 65'536> buffer{};
        udp::endpoint endpoint;
        std::size_t received{};
        for (;;)
        {
            boost::system::error_code error;
            const auto bytes = receiver.receive_from(boost::asio::buffer(buffer), endpoint, 0, error);
            if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
            {
                break;
            }
            require(!error && bytes == 60'000 && buffer.front() == 0, "pending datagram submitted before shutdown");
            ++received;
        }
        require(received <= 1, "concurrent async_send_to escaped queue serialization");
    }
}

void overflow_and_reentrant_write()
{
    boost::asio::io_context io;
    auto sender = bound(io);
    auto receiver = bound(io);
    const auto endpoint = address(receiver);
    std::size_t completed{};
    std::size_t rejections{};
    std::size_t received{};
    sender->set_write_callback([&](boost::system::error_code error, std::size_t bytes)
                              {
                                  require(!error && bytes == (completed < 32 ? 32'768U : 257U), "overflow damaged existing queue");
                                  ++completed;
                                  if (completed == 32)
                                  {
                                      require(sender->write(std::vector<std::uint8_t>(257, 32), endpoint), "overflow stopped writer");
                                  }
                                  if (completed == 33)
                                  {
                                      sender->shutdown();
                                  }
                              });
    boost::asio::spawn(io, [&](boost::asio::yield_context yield)
                      {
                          std::array<std::uint8_t, 65'536> buffer{};
                          udp::endpoint remote;
                          for (std::uint8_t sequence = 0; sequence < 33; ++sequence)
                          {
                              boost::system::error_code error;
                              const auto bytes = receiver->read(buffer, remote, yield, error);
                              require(!error && bytes == (sequence < 32 ? 32'768U : 257U), "overflow lost accepted datagram");
                              for (std::size_t index = 0; index < bytes; ++index)
                              {
                                  require(buffer[index] == sequence, "overflow corrupted packet/order");
                              }
                              ++received;
                          }
                      }, rethrow);
    boost::asio::post(io, [&]()
                      {
                          for (std::uint8_t sequence = 0; sequence < 32; ++sequence)
                          {
                              require(sender->write(std::vector<std::uint8_t>(32'768, sequence), endpoint), "1 MiB inclusive limit changed");
                          }
                          require(!sender->write(std::vector<std::uint8_t>{0xff}, endpoint), "overflow newest accepted");
                          ++rejections;
                          require(!sender->write(std::vector<std::uint8_t>(1'048'577, 0xff), endpoint), "oversized queue item accepted");
                          ++rejections;
                          require(rejections == 2 && completed == 0, "overflow triggered send completion");
                      });
    drain(io);
    require(completed == 33 && received == 33 && rejections == 2, "overflow recovery failed");
}

void socket_error_and_reconstruction()
{
    boost::asio::io_context io;
    auto sender = bound(io);
    const std::weak_ptr<udp_transport> error_lifetime = sender;
    udp::socket receiver(io, udp::endpoint(loopback, 0));
    receiver.non_blocking(true);
    const auto endpoint = receiver.local_endpoint();
    auto callback_owner = std::make_shared<int>(0);
    const std::weak_ptr<int> callback_lifetime = callback_owner;
    std::size_t errors{};
    bool owner_closed{};
    sender->set_write_callback([&, callback_owner](boost::system::error_code error, std::size_t bytes)
                              {
                                  require(error == boost::asio::error::message_size && bytes == 0, "real send error not propagated");
                                  ++errors;
                                  require(address(sender).port() != 0, "send error closed socket before owner shutdown");
                                  require(sender->write(std::vector<std::uint8_t>{3}, endpoint), "send error made transport terminal before owner shutdown");
                                  // 上层稍后关闭；失败包仍在队首，因此新包也不能开始发送。
                                  boost::asio::post(io, [&]()
                                                    {
                                                        require(errors == 1 && !callback_lifetime.expired(), "send error cleared callback before owner shutdown");
                                                        sender->shutdown();
                                                        require(callback_lifetime.expired(), "owner shutdown retained write callback");
                                                        require(!sender->write(std::vector<std::uint8_t>{4}, endpoint), "write after owner shutdown accepted");
                                                        sender.reset();
                                                        owner_closed = true;
                                                    });
                              });
    callback_owner.reset();
    boost::asio::post(io, [&]()
                      {
                          require(sender->write(std::vector<std::uint8_t>(65'508, 1), endpoint), "socket-error packet not admitted");
                          require(sender->write(std::vector<std::uint8_t>{2}, endpoint), "socket-error pending packet not admitted");
                      });
    drain(io);
    require(owner_closed && errors == 1 && error_lifetime.expired(), "socket error completion repeated or transport retained");
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint remote;
    boost::system::error_code read_error;
    const auto unexpected_bytes = receiver.receive_from(boost::asio::buffer(buffer), remote, 0, read_error);
    require(unexpected_bytes == 0 && (read_error == boost::asio::error::would_block || read_error == boost::asio::error::try_again),
            "pending datagram sent after socket error");

    io.restart();
    sender = bound(io);
    const std::weak_ptr<udp_transport> old_lifetime = sender;
    std::shared_ptr<udp_transport> replacement;
    std::weak_ptr<udp_transport> replacement_lifetime;
    std::size_t old_completions{};
    std::size_t completions{};
    boost::asio::post(io, [&]()
                      {
                          sender->set_write_callback([&](boost::system::error_code, std::size_t) { ++old_completions; });
                          require(sender->write(std::vector<std::uint8_t>(1024, 3), endpoint), "old datagram not admitted");
                          require(sender->write(std::vector<std::uint8_t>{5}, endpoint), "old pending datagram not admitted");
                          sender->shutdown();
                          require(!sender->write(std::vector<std::uint8_t>{6}, endpoint), "closed old transport accepted write");
                          sender.reset();
                          require(!old_lifetime.expired(), "old transport released before in-flight completion");
                          replacement = bound(io);
                          replacement_lifetime = replacement;
                          replacement->set_write_callback([&](boost::system::error_code result, std::size_t bytes)
                                                          {
                                                              require(!result && bytes == 17, "old completion affected replacement transport");
                                                              ++completions;
                                                              replacement->shutdown();
                                                              require(!replacement->write(std::vector<std::uint8_t>{7}, endpoint),
                                                                      "closed replacement accepted write");
                                                              replacement.reset();
                                                          });
                          require(replacement->write(std::vector<std::uint8_t>(17, 4), endpoint), "replacement enqueue rejected");
                      });
    drain(io);
    require(old_completions == 0 && completions == 1 && old_lifetime.expired() && replacement_lifetime.expired(),
            "reconstruction completion or transport lifetime changed");
    std::size_t old_packets{};
    std::size_t replacement_packets{};
    for (;;)
    {
        const auto bytes = receiver.receive_from(boost::asio::buffer(buffer), remote, 0, read_error);
        if (read_error == boost::asio::error::would_block || read_error == boost::asio::error::try_again)
        {
            break;
        }
        require(!read_error, "reconstruction receive failed");
        if (bytes == 1024 && buffer.front() == 3)
        {
            ++old_packets;
        }
        else
        {
            require(bytes == 17 && buffer.front() == 4, "closed transport sent a pending datagram");
            ++replacement_packets;
        }
    }
    require(old_packets <= 1 && replacement_packets == 1, "replacement datagram missing or old write repeated");
}

void gb_session_error_policies()
{
    media_port_pool::init(default_media_port_start, default_media_port_end);
    for (const bool overflow : {true, false})
    {
        worker_context worker;
        auto receiver = bound(worker.io());
        std::size_t received{};
        std::size_t before_recovery{};
        std::ostringstream logs;
        auto logger = spdlog::default_logger();
        const auto previous_level = logger->level();
        const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logs);
        logger->sinks().push_back(sink);
        logger->set_level(spdlog::level::debug);
        boost::scope::scope_exit restore_logger([&]()
                                               {
                                                   logger->sinks().pop_back();
                                                   logger->set_level(previous_level);
                                               });
        auto stream = std::make_shared<media_stream>(overflow ? "verify/overflow" : "verify/error", worker);
        require(stream->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                     .clock_rate = 90'000, .channel_count = 0, .codec_config = {}}}), "GB test tracks failed");
        auto endpoint = address(receiver);
        if (!overflow)
        {
            endpoint.port(0);
        }
        auto sender = std::make_shared<gb28181_udp_sender_session>(worker, stream, "sender", endpoint, std::nullopt);
        const std::weak_ptr<gb28181_udp_sender_session> lifetime = sender;
        boost::asio::steady_timer publish(worker.io()), check(worker.io()), finish(worker.io());
        boost::asio::spawn(worker.io(), [&](boost::asio::yield_context yield)
                          {
                              std::array<std::uint8_t, 2048> buffer{};
                              udp::endpoint remote;
                              for (;;)
                              {
                                  boost::system::error_code error;
                                  receiver->read(buffer, remote, yield, error);
                                  if (error)
                                  {
                                      require(error == boost::asio::error::operation_aborted, "GB receive failed");
                                      return;
                                  }
                                  ++received;
                              }
                          }, rethrow);
        boost::asio::post(worker.io(), [&]()
                          {
                              require(sender->startup(loopback, 96, 1234), "GB sender startup failed");
                              require(session_registry::instance().add_sender_session(stream->stream_id(), "sender", sender),
                                      "GB sender registry failed");
                              publish.expires_after(10ms);
                              publish.async_wait([&](boost::system::error_code error)
                                                 {
                                                     require(!error, "GB publish timer cancelled");
                                                     stream->publish({.track = 1, .key_frame = true, .payload = std::make_shared<const std::vector<std::uint8_t>>(
                                                                                      overflow ? 2'097'152U : 160U, 0xd5)});
                                                 });
                              check.expires_after(200ms);
                              check.async_wait([&](boost::system::error_code error)
                                               {
                                                   require(!error, "GB check timer cancelled");
                                                   const auto registered = session_registry::instance().take_sender_session(stream->stream_id(), "sender");
                                                   require(overflow ? registered == sender : !registered, "GB overflow/socket-error shutdown policy changed");
                                                   before_recovery = received;
                                                   if (overflow)
                                                   {
                                                       require(logs.str().find("udp write queue full") != std::string::npos, "GB overflow was not exercised");
                                                       stream->publish({.track = 1, .pts_ns = 20'000'000,
                                                                        .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5)});
                                                   }
                                               });
                              finish.expires_after(250ms);
                              finish.async_wait([&](boost::system::error_code error)
                                                {
                                                    require(!error, "GB finish timer cancelled");
                                                    require(!overflow || received > before_recovery, "GB stopped media after overflow");
                                                    sender->shutdown();
                                                    sender->shutdown();
                                                    sender.reset();
                                                    receiver->shutdown();
                                                    stream->end();
                                                    worker.request_stop();
                                                });
                          });
        drain(worker.io());
        require(lifetime.expired(), "GB sender retained after shutdown");
        const auto text = logs.str();
        const auto shutdown = text.find("gb28181 udp sender shutdown");
        require(shutdown != std::string::npos && text.find("gb28181 udp sender shutdown", shutdown + 1U) == std::string::npos,
                "GB shutdown missing/duplicated");
    }
}
}    // namespace

int main()
{
    try
    {
        serialization_and_endpoints();
        in_flight_shutdown();
        overflow_and_reentrant_write();
        socket_error_and_reconstruction();
        gb_session_error_policies();
        std::cout << "UDP serialization, endpoints, shutdown, overflow, socket error and GB policies: PASS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
