#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <future>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <boost/asio/post.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/net/port_manager.h"
#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

extern "C"
{
#include "rtp-packet.h"
}

namespace media_server
{
namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Handler>
void run_on_owner(worker_context& worker, Handler&& handler)
{
    bool completed = false;
    boost::asio::post(worker.io(),
                      [&handler, &completed]()
                      {
                          handler();
                          completed = true;
                      });
    while (!completed)
    {
        if (worker.io().stopped())
        {
            worker.io().restart();
        }
        static_cast<void>(worker.io().run_one());
    }
}

void test_udp_sender_session_sends_rtp()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();

    boost::asio::ip::udp::socket rtp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    constexpr track_id video_track_id = 1;
    const std::vector<std::uint8_t> config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    auto source = std::make_shared<media_stream>("live/gb-udp-sender-session", worker);
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = config,
            }}),
            "gb udp sender source tracks");
    require(stream_registry::instance().add(source), "gb udp sender source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    constexpr std::string_view stream_id = "550e8400-e29b-41d4-a716-446655440000";
    auto session = std::make_shared<gb28181_udp_sender_session>(worker,
                                                                std::string{stream_id},
                                                                source,
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                "udp-sender",
                                                                false,
                                                                std::chrono::milliseconds{25'000},
                                                                1024U * 1024U);
    require(stream_registry::instance().add_sender_session(source->name(), "udp-sender", session), "gb udp sender session registry");
    bool started = false;
    run_on_owner(worker, [&]() { started = session->startup(); });
    require(started, "gb udp sender session startup");

    io.run_for(std::chrono::milliseconds(20));
    io.restart();

    auto payload = config;
    payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
    source->publish(media_frame{
        .track = video_track_id,
        .dts_ns = 0,
        .pts_ns = 0,
        .key_frame = true,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (rtp_receiver.available() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(rtp_receiver.available() > 0, "gb udp sender sends RTP");

    std::array<std::uint8_t, 2048> packet{};
    boost::asio::ip::udp::endpoint sender;
    const auto bytes = rtp_receiver.receive_from(boost::asio::buffer(packet), sender);
    rtp_packet_t decoded{};
    require(rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(bytes)) == 0 && decoded.payloadlen > 0, "gb udp sender sends valid RTP");

    run_on_owner(worker, [&]() { session->shutdown(); });
    io.run();
}

void test_udp_sender_queue_overflow_drops_packet()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();

    boost::asio::ip::udp::socket rtp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr track_id video_track_id = 1;
    const std::vector<std::uint8_t> config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    auto source = std::make_shared<media_stream>("live/gb-udp-overflow", worker);
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = config,
            }}),
            "gb udp overflow source tracks");
    require(stream_registry::instance().add(source), "gb udp overflow source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = 96,
        .ssrc = 0x12345680U,
    };
    constexpr std::string_view stream_id = "550e8400-e29b-41d4-a716-446655440000";
    auto session = std::make_shared<gb28181_udp_sender_session>(worker,
                                                                std::string{stream_id},
                                                                source,
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                "udp-overflow",
                                                                false,
                                                                std::chrono::milliseconds{25'000},
                                                                0U);
    require(stream_registry::instance().add_sender_session(source->name(), "udp-overflow", session), "gb udp overflow sender registry");
    bool started = false;
    run_on_owner(worker, [&]() { started = session->startup(); });
    require(started, "gb udp overflow session startup");

    io.run_for(std::chrono::milliseconds(20));
    io.restart();

    auto payload = config;
    payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
    source->publish(media_frame{
        .track = video_track_id,
        .dts_ns = 0,
        .pts_ns = 0,
        .key_frame = true,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
    });
    io.run_for(std::chrono::milliseconds(40));
    io.restart();

    require(rtp_receiver.available() == 0U, "gb udp overflow drops new packet");
    auto registered = stream_registry::instance().take_sender_session(source->name(), "udp-overflow");
    require(registered.get() == session.get(), "gb udp overflow keeps session running");

    std::weak_ptr<gb28181_udp_sender_session> weak_session = session;
    run_on_owner(worker, [&]() { session->shutdown(); });
    session.reset();
    registered.reset();
    io.run();
    require(weak_session.expired(), "gb udp overflow shutdown releases session");
}

void test_udp_sender_rtcp_shutdown_releases_scheduler()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();

    boost::asio::ip::udp::socket rtp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345679U;
    constexpr track_id video_track_id = 1;
    const std::vector<std::uint8_t> config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    auto source = std::make_shared<media_stream>("live/gb-udp-sender-rtcp-shutdown", worker);
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = config,
            }}),
            "gb udp sender rtcp source tracks");
    require(stream_registry::instance().add(source), "gb udp sender rtcp source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_sender_session>(worker,
                                                                "550e8400-e29b-41d4-a716-446655440000",
                                                                source,
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                "udp-sender-rtcp",
                                                                true,
                                                                std::chrono::milliseconds::zero());
    require(stream_registry::instance().add_sender_session(source->name(), "udp-sender-rtcp", session), "gb udp sender rtcp session registry");
    require(session->startup(), "gb udp sender rtcp session startup");

    io.run_for(std::chrono::milliseconds(20));
    io.restart();

    auto payload = config;
    payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
    source->publish(media_frame{
        .track = video_track_id,
        .dts_ns = 0,
        .pts_ns = 0,
        .key_frame = true,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
    });

    session->shutdown();
    std::weak_ptr<gb28181_udp_sender_session> weak_session = session;
    session.reset();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!weak_session.expired() && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(weak_session.expired(), "gb udp sender rtcp scheduler released after shutdown");
}

void test_udp_sender_worker_stop_releases_idle_session()
{
    worker_context worker;
    auto& io = worker.io();
    boost::asio::ip::udp::socket rtp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_receiver(io, {boost::asio::ip::address_v4::loopback(), 0});
    auto source = std::make_shared<media_stream>("live/gb-udp-worker-stop", worker);
    require(source->set_tracks({media_track{
                .id = 1,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
                                 0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80},
            }}),
            "gb udp worker stop source tracks");
    require(stream_registry::instance().add(source), "gb udp worker stop source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = 96,
        .ssrc = 0x12345681U,
    };
    auto session = std::make_shared<gb28181_udp_sender_session>(worker,
                                                                "550e8400-e29b-41d4-a716-446655440000",
                                                                source,
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                "udp-worker-stop",
                                                                true,
                                                                std::chrono::hours{1});
    require(stream_registry::instance().add_sender_session(source->name(), "udp-worker-stop", session), "gb udp worker stop session registry");

    std::promise<bool> started_signal;
    auto started = started_signal.get_future();
    std::promise<void> returned_signal;
    auto returned = returned_signal.get_future();
    boost::asio::post(worker.io(), [&]() { started_signal.set_value(session->startup()); });
    std::jthread runner([&]() {
        worker.run();
        returned_signal.set_value();
    });
    require(started.get(), "gb udp worker stop session startup");

    worker.request_stop();
    const auto returned_in_time = returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!returned_in_time)
    {
        worker.stop();
    }
    runner.join();

    require(returned_in_time, "worker stop drains idle gb udp sender");
    require(!stream_registry::instance().take_sender_session(source->name(), "udp-worker-stop"), "worker stop removes gb udp sender registry entry");
    session.reset();
    stream_registry::instance().remove(*source);
}

}    // namespace
}    // namespace media_server

int main(int argc, char* argv[])
{
    media_server::port_manager::init(32'500, 32'599);
    try
    {
        if (argc != 2)
        {
            throw std::runtime_error("gb28181 UDP sender test scenario required");
        }
        const std::string_view scenario{argv[1]};
        if (scenario == "sends_rtp")
        {
            media_server::test_udp_sender_session_sends_rtp();
        }
        else if (scenario == "queue_overflow")
        {
            media_server::test_udp_sender_queue_overflow_drops_packet();
        }
    else if (scenario == "rtcp_shutdown")
    {
        media_server::test_udp_sender_rtcp_shutdown_releases_scheduler();
    }
    else if (scenario == "worker_stop")
    {
        media_server::test_udp_sender_worker_stop_releases_idle_session();
    }
        else
        {
            throw std::runtime_error("unknown gb28181 UDP sender test scenario");
        }
        std::cout << "[pass] " << scenario << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] gb28181_udp_sender_tests: " << error.what() << '\n';
        return 1;
    }
}
