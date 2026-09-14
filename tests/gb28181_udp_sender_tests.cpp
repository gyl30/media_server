#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_sender_session.h"
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"

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

struct captured_events
{
    std::vector<runtime_event> values;
    bool owner_only{true};
};

runtime_event_emitter_ptr capture_events(worker_context& worker, captured_events& events)
{
    return std::make_shared<runtime_event_emitter>(
        "media-1",
        "instance-1",
        [&worker, &events](runtime_event event)
        {
            events.owner_only = events.owner_only && worker.io().get_executor().running_in_this_thread();
            events.values.push_back(std::move(event));
        });
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

void require_gb_event(const runtime_event& event,
                      runtime_event_type type,
                      std::string_view stream_id,
                      std::string_view stream_name,
                      runtime_state state,
                      std::string_view stage,
                      std::string_view message)
{
    const bool stage_matches = stage.empty() ? !event.stage : event.stage == stage;
    require(event.type == type && event.server_id == "media-1" && event.instance_id == "instance-1" &&
                event.stream_id == stream_id && event.stream_name == stream_name && !event.source_id &&
                event.direction == runtime_direction::output && event.protocol == runtime_protocol::gb28181 && event.state == state &&
                stage_matches,
            message);
}

void test_udp_sender_session_sends_rtp()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();

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
    require(streams.add(source), "gb udp sender source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    constexpr std::string_view stream_id = "550e8400-e29b-41d4-a716-446655440000";
    captured_events events;
    auto session = std::make_shared<gb28181_udp_sender_session>(
        worker,
        std::string{stream_id},
        source,
        description,
        boost::asio::ip::address_v4::loopback(),
        "udp-sender",
        false,
        std::chrono::milliseconds{25'000},
        1024U * 1024U,
        capture_events(worker, events));
    require(streams.add_sender_session(source->name(), "udp-sender", session), "gb udp sender session registry");
    bool started = false;
    run_on_owner(worker, [&]() { started = session->startup(); });
    require(started, "gb udp sender session startup");
    require(events.values.size() == 1U, "gb udp sender starting event count");
    require_gb_event(events.values[0],
                     runtime_event_type::output_started,
                     stream_id,
                     source->name(),
                     runtime_state::starting,
                     {},
                     "gb udp sender starting event payload");

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
    require(events.values.size() == 2U, "gb udp sender streaming event count");
    require_gb_event(events.values[1],
                     runtime_event_type::output_started,
                     stream_id,
                     source->name(),
                     runtime_state::streaming,
                     "streaming",
                     "gb udp sender streaming event payload");

    std::array<std::uint8_t, 2048> packet{};
    boost::asio::ip::udp::endpoint sender;
    const auto bytes = rtp_receiver.receive_from(boost::asio::buffer(packet), sender);
    rtp_packet_t decoded{};
    require(rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(bytes)) == 0 && decoded.payloadlen > 0,
            "gb udp sender sends valid RTP");

    run_on_owner(worker, [&]() { session->shutdown(); });
    io.run();
    require(events.values.size() == 3U, "gb udp sender stopped event count");
    require_gb_event(events.values[2],
                     runtime_event_type::output_stopped,
                     stream_id,
                     source->name(),
                     runtime_state::stopped,
                     {},
                     "gb udp sender stopped event payload");
    require(events.values[2].end_reason == runtime_end_reason::requested && !events.values[2].error,
            "gb udp sender stopped event reason");
    require(events.owner_only, "gb udp sender events emitted on owner worker");
}

void test_udp_sender_queue_overflow_drops_packet()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();

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
    require(streams.add(source), "gb udp overflow source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = 96,
        .ssrc = 0x12345680U,
    };
    constexpr std::string_view stream_id = "550e8400-e29b-41d4-a716-446655440000";
    captured_events events;
    auto session = std::make_shared<gb28181_udp_sender_session>(
        worker,
        std::string{stream_id},
        source,
        description,
        boost::asio::ip::address_v4::loopback(),
        "udp-overflow",
        false,
        std::chrono::milliseconds{25'000},
        0U,
        capture_events(worker, events));
    require(streams.add_sender_session(source->name(), "udp-overflow", session), "gb udp overflow sender registry");
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
    require(events.values.size() == 1U, "gb udp overflow does not report streaming or stop");
    require_gb_event(events.values[0],
                     runtime_event_type::output_started,
                     stream_id,
                     source->name(),
                     runtime_state::starting,
                     {},
                     "gb udp overflow starting event payload");
    auto registered = streams.take_sender_session(source->name(), "udp-overflow");
    require(registered.get() == session.get(), "gb udp overflow keeps session running");

    std::weak_ptr<gb28181_udp_sender_session> weak_session = session;
    run_on_owner(worker, [&]() { session->shutdown(); });
    session.reset();
    registered.reset();
    io.run();
    require(weak_session.expired(), "gb udp overflow shutdown releases session");
    require(events.values.size() == 2U, "gb udp overflow terminal event count");
    require_gb_event(events.values[1],
                     runtime_event_type::output_stopped,
                     stream_id,
                     source->name(),
                     runtime_state::stopped,
                     {},
                     "gb udp overflow stopped event payload");
    require(events.values[1].end_reason == runtime_end_reason::requested && !events.values[1].error,
            "gb udp overflow remains normally stoppable");
    require(events.owner_only, "gb udp overflow events emitted on owner worker");
}

void test_udp_sender_rtcp_shutdown_releases_scheduler()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();

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
    require(streams.add(source), "gb udp sender rtcp source registry");

    const gb28181_transport_config description{
        .mode = gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_sender_session>(worker,
                                                                "550e8400-e29b-41d4-a716-446655440000", source,
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                "udp-sender-rtcp",
                                                                true,
                                                                std::chrono::milliseconds::zero());
    require(streams.add_sender_session(source->name(), "udp-sender-rtcp", session), "gb udp sender rtcp session registry");
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

}    // namespace
}    // namespace media_server

int main()
{
    media_server::port_manager::init(32'500, 32'599);
    media_server::stream_registry::instance().clear();
    try
    {
        for (int iteration = 0; iteration < 10; ++iteration)
        {
            media_server::test_udp_sender_session_sends_rtp();
            media_server::test_udp_sender_queue_overflow_drops_packet();
            media_server::test_udp_sender_rtcp_shutdown_releases_scheduler();
        }
        media_server::stream_registry::instance().clear();
        media_server::port_manager::destroy();
        std::cout << "[pass] gb28181_udp_sender_tests\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::stream_registry::instance().clear();
        media_server::port_manager::destroy();
        std::cerr << "[fail] gb28181_udp_sender_tests: " << error.what() << '\n';
        return 1;
    }
}
