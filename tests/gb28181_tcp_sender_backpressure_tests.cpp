#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace
{

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

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

void test_tcp_sender_write_backlog_limit()
{
    worker_context worker;
    auto& io = worker.io();
    auto& streams = registry::instance();
    streams.clear();

    constexpr track_id video_track_id = 1;
    const std::vector<std::uint8_t> config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    auto source = std::make_shared<media_stream>("live/gb-tcp-backpressure", worker);
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = config,
            }}),
            "gb tcp backpressure source tracks");
    require(streams.add(source), "gb tcp backpressure source registry");

    boost::asio::io_context peer_io;
    tcp::acceptor receiver(peer_io, {boost::asio::ip::address_v4::loopback(), 0});
    const gb28181_transport_config description{
        .mode = gb28181_transport::tcp_active,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_port = receiver.local_endpoint().port(),
        .payload_type = 96,
        .ssrc = 0x12345678U,
    };
    auto session = std::make_shared<gb28181_tcp_sender_session>(worker,
                                                               std::weak_ptr<media_stream>{source},
                                                               source->name(),
                                                               "backpressure",
                                                               description,
                                                               boost::asio::ip::address_v4::loopback(),
                                                               1s,
                                                               0U);
    require(streams.add_sender_session(source->name(), "backpressure", session), "gb tcp backpressure session registry");
    require(session->startup(), "gb tcp backpressure startup");

    worker.release_work();
    std::jthread runner([&worker]() { worker.run(); });

    tcp::socket peer(peer_io);
    boost::system::error_code error;
    receiver.accept(peer, error);
    require(!error, "gb tcp backpressure connection");

    auto payload = config;
    payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
    boost::asio::post(io,
                      [source, payload = std::move(payload)]() mutable
                      {
                          source->publish(media_frame{
                              .track = video_track_id,
                              .dts_ns = 0,
                              .pts_ns = 0,
                              .key_frame = true,
                              .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
                          });
                      });

    const bool closed = wait_for_close(peer, 1s);
    session->shutdown();
    runner.join();
    require(closed, "gb tcp write backlog limit closes connection");
    streams.clear();
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::registry::init();
    try
    {
        media_server::test_tcp_sender_write_backlog_limit();
        media_server::registry::destroy();
        std::cout << "[pass] gb28181_tcp_sender_write_backlog_limit\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        std::cerr << "[fail] gb28181_tcp_sender_write_backlog_limit: " << error.what() << '\n';
        return 1;
    }
}
