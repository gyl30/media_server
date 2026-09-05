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

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_output_session.h"
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

void test_udp_output_session_sends_rtp()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = registry::instance();
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
    auto source = std::make_shared<media_stream>("live/gb-udp-output-session", io.get_executor());
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = config,
            }}),
            "gb udp output source tracks");
    require(streams.add(source), "gb udp output source registry");

    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .rtp_port = rtp_receiver.local_endpoint().port(),
        .rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_output_session>(
        worker, source, description, boost::asio::ip::address_v4::loopback(), "udp-output", false);
    require(streams.add_output_session(source->name(), "udp-output", session), "gb udp output session registry");
    require(session->startup(), "gb udp output session startup");

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
    require(rtp_receiver.available() > 0, "gb udp output sends RTP");

    std::array<std::uint8_t, 2048> packet{};
    boost::asio::ip::udp::endpoint sender;
    const auto bytes = rtp_receiver.receive_from(boost::asio::buffer(packet), sender);
    rtp_packet_t decoded{};
    require(rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(bytes)) == 0 && decoded.payloadlen > 0,
            "gb udp output sends valid RTP");

    session->shutdown();
    io.restart();
    io.run();
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::port_manager::init(32'500, 32'599);
    media_server::registry::init();
    try
    {
        media_server::test_udp_output_session_sends_rtp();
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cout << "[pass] gb28181_udp_output_tests\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cerr << "[fail] gb28181_udp_output_tests: " << error.what() << '\n';
        return 1;
    }
}
