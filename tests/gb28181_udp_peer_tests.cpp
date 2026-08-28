#include <array>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_udp_session.h"

extern "C"
{
#include "rtsp-muxer.h"
#include "rtp-packet.h"
#include "rtp-profile.h"
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

std::uint16_t unused_udp_port(boost::asio::io_context& io)
{
    boost::asio::ip::udp::socket socket(io, {boost::asio::ip::address_v4::loopback(), 0});
    return socket.local_endpoint().port();
}

std::vector<std::vector<std::uint8_t>> make_ps_rtp(std::uint8_t payload_type, std::uint32_t ssrc)
{
    std::vector<std::vector<std::uint8_t>> packets;
    const auto collect = +[](void* param, int, const void* packet, int bytes, std::uint32_t, int)
    {
        auto& output = *static_cast<std::vector<std::vector<std::uint8_t>>*>(param);
        const auto* begin = static_cast<const std::uint8_t*>(packet);
        output.emplace_back(begin, begin + bytes);
        return 0;
    };

    const std::vector<std::uint8_t> config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    auto* muxer = rtsp_muxer_create(collect, &packets);
    require(muxer != nullptr, "gb peer ps muxer create");
    const auto payload =
        rtsp_muxer_add_payload(muxer, "RTP/AVP", 90'000, payload_type, "PS", 1, ssrc, 0, config.data(), static_cast<int>(config.size()));
    require(payload >= 0, "gb peer ps payload");
    const auto media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_H264, config.data(), static_cast<int>(config.size()));
    require(media >= 0, "gb peer ps media");

    auto frame = config;
    const std::array<std::uint8_t, 9> idr{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0};
    frame.insert(frame.end(), idr.begin(), idr.end());
    require(rtsp_muxer_input(muxer, media, 0, 0, frame.data(), static_cast<int>(frame.size()), 1) == 0, "gb peer ps first frame");
    const std::array<std::uint8_t, 8> p_frame{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
    require(rtsp_muxer_input(muxer, media, 40, 40, p_frame.data(), static_cast<int>(p_frame.size()), 0) == 0, "gb peer ps second frame");
    require(rtsp_muxer_destroy(muxer) == 0 && !packets.empty(), "gb peer ps packets");
    return packets;
}

void send_packets(boost::asio::ip::udp::socket& socket, std::uint16_t port, const std::vector<std::vector<std::uint8_t>>& packets)
{
    const boost::asio::ip::udp::endpoint target{boost::asio::ip::address_v4::loopback(), port};
    for (const auto& packet : packets)
    {
        socket.send_to(boost::asio::buffer(packet), target);
    }
}

void test_ps_fixture_creates_stream()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    gb28181_input_media media(io.get_executor(), "live/gb-peer-fixture", payload_type, ssrc);
    require(media.startup(), "gb peer fixture media startup");
    const auto packets = make_ps_rtp(payload_type, ssrc);
    for (const auto& packet : packets)
    {
        require(media.input_rtp(packet) == gb28181_rtp_input_result::accepted, "gb peer fixture packet accepted");
    }
    require(streams.find("live/gb-peer-fixture") != nullptr, "gb peer fixture creates stream");
    media.shutdown();
}

void test_output_same_codec_config_version_continues_ps_stream()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();

    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    constexpr track_id video_track_id = 1;
    const std::vector<std::uint8_t> initial_config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    const std::vector<std::uint8_t> updated_config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xba, 0x6a, 0x02, 0x1a, 0x02, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8c, 0x18, 0xcb, 0x00, 0x00, 0x00, 0x01, 0x68, 0xef, 0xbc, 0xb0,
    };
    const auto make_frame = [](std::int64_t pts_ns, bool key_frame, const std::vector<std::uint8_t>& config)
    {
        std::vector<std::uint8_t> payload;
        if (key_frame)
        {
            payload = config;
            payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
        }
        else
        {
            payload = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
        }
        return media_frame{
            .track = video_track_id,
            .dts_ns = pts_ns,
            .pts_ns = pts_ns,
            .key_frame = key_frame,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
        };
    };

    auto source = std::make_shared<media_stream>("live/gb-output-config-source", io.get_executor());
    require(source->set_tracks({media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = initial_config,
            }}),
            "gb output config initial track");

    gb28181_input_media receiver(io.get_executor(), "live/gb-output-config-received", payload_type, ssrc);
    require(receiver.startup(), "gb output config receiver startup");

    std::size_t packet_count = 0;
    std::size_t end_count = 0;
    std::vector<std::uint8_t> ps_payload;
    auto output = std::make_shared<gb28181_output_media>(
        io.get_executor(),
        source,
        payload_type,
        ssrc,
        [&](std::vector<std::uint8_t> packet)
        {
            ++packet_count;
            rtp_packet_t decoded{};
            require(rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(packet.size())) == 0, "gb output config rtp packet");
            const auto* begin = static_cast<const std::uint8_t*>(decoded.payload);
            ps_payload.insert(ps_payload.end(), begin, begin + decoded.payloadlen);
            require(receiver.input_rtp(packet) == gb28181_rtp_input_result::accepted, "gb output config packet accepted");
        },
        [&end_count]() { ++end_count; });
    require(output->startup(), "gb output config startup");

    const auto initial_track = source->tracks().front();
    auto initial_snapshot = std::make_shared<media_track_snapshot>();
    initial_snapshot->revision = 1;
    initial_snapshot->tracks = {initial_track};
    output->on_read(media_read_batch{
        .next_cursor = 2,
        .tracks = initial_snapshot,
        .entries = {
            {.config_version = initial_track.config_version, .frame = make_frame(0, true, initial_config)},
            {.config_version = initial_track.config_version, .frame = make_frame(40'000'000, false, initial_config)},
        },
    });
    require(packet_count > 0U, "gb output config initial ps output");
    const auto received = streams.find("live/gb-output-config-received");
    require(received != nullptr, "gb output config receiver initial stream");

    auto updated_track = initial_track;
    updated_track.codec_config = updated_config;
    require(source->update_track(std::move(updated_track)), "gb output config update source track");
    updated_track = source->tracks().front();
    require(updated_track.config_version == 2, "gb output config source generation increments");

    auto updated_snapshot = std::make_shared<media_track_snapshot>();
    updated_snapshot->revision = 2;
    updated_snapshot->tracks = {updated_track};
    output->on_tracks(updated_snapshot);

    const auto before_resync_packets = packet_count;
    output->on_read(media_read_batch{
        .next_cursor = 4,
        .tracks = updated_snapshot,
        .entries = {
            {.config_version = 1, .frame = make_frame(80'000'000, true, initial_config)},
            {.config_version = updated_track.config_version, .frame = make_frame(120'000'000, false, updated_config)},
        },
    });
    require(packet_count == before_resync_packets, "gb output config drops stale generation and waits for key frame");
    require(end_count == 0U, "gb output config same codec update stays open");

    ps_payload.clear();
    output->on_read(media_read_batch{
        .next_cursor = 5,
        .tracks = updated_snapshot,
        .entries = {{.config_version = updated_track.config_version, .frame = make_frame(160'000'000, true, updated_config)}},
    });
    require(packet_count > before_resync_packets, "gb output config resumes existing ps stream on new key frame");
    require(end_count == 0U, "gb output config remains open after resync");
    require(std::search(ps_payload.begin(), ps_payload.end(), updated_config.begin(), updated_config.end()) != ps_payload.end(),
            "gb output config updated parameter sets stay in ps payload");
    require(streams.find("live/gb-output-config-received") == received, "gb output config receiver stream stays active");

    output->shutdown();
    receiver.shutdown();
    io.run();
}

void test_signaled_rtp_peer_is_pinned_before_first_packet()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();
    boost::asio::ip::udp::socket allowed(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket blocked(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket remote_rtcp(io, {boost::asio::ip::address_v4::loopback(), 0});

    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    const auto local_rtp_port = unused_udp_port(io);
    auto local_rtcp_port = unused_udp_port(io);
    while (local_rtcp_port == local_rtp_port)
    {
        local_rtcp_port = unused_udp_port(io);
    }

    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .rtp_port = local_rtp_port,
        .rtcp_port = local_rtcp_port,
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    const gb28181_udp_peer peer{
        .rtp = allowed.local_endpoint(),
        .rtcp_port = remote_rtcp.local_endpoint().port(),
    };
    auto session = std::make_shared<gb28181_udp_session>(io.get_executor(), "live/gb-peer-signaled", description, peer);
    require(session->startup(), "gb peer signaled startup");

    const auto packets = make_ps_rtp(payload_type, ssrc);
    send_packets(blocked, local_rtp_port, packets);
    io.run_for(std::chrono::milliseconds(200));
    require(!streams.find("live/gb-peer-signaled"), "gb peer signaled rejects other first source");

    io.restart();
    send_packets(allowed, local_rtp_port, packets);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!streams.find("live/gb-peer-signaled") && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(streams.find("live/gb-peer-signaled") != nullptr, "gb peer signaled accepts configured source");

    boost::asio::ip::udp::socket other_rtcp(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr std::array<std::uint8_t, 28> sender_report{
        0x80, 0xc8, 0x00, 0x06, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    other_rtcp.send_to(boost::asio::buffer(sender_report), {boost::asio::ip::address_v4::loopback(), local_rtcp_port});

    io.restart();
    io.run_for(std::chrono::milliseconds(4500));
    require(remote_rtcp.available() > 0, "gb peer signaled keeps rtcp target on configured port");
    require(other_rtcp.available() == 0, "gb peer signaled never rebinds rtcp target");

    session->shutdown();
    io.restart();
    io.run();
}

void test_first_valid_rtp_packet_pins_peer_when_unsignaled()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();
    boost::asio::ip::udp::socket expected(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket wrong(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket remote_rtcp(io, {boost::asio::ip::address_v4::loopback(), 0});

    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    const auto local_rtp_port = unused_udp_port(io);
    auto local_rtcp_port = unused_udp_port(io);
    while (local_rtcp_port == local_rtp_port)
    {
        local_rtcp_port = unused_udp_port(io);
    }

    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .rtp_port = local_rtp_port,
        .rtcp_port = local_rtcp_port,
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    const gb28181_udp_peer peer{
        .rtp = std::nullopt,
        .rtcp_port = remote_rtcp.local_endpoint().port(),
    };
    auto session = std::make_shared<gb28181_udp_session>(io.get_executor(), "live/gb-peer-learned", description, peer);
    require(session->startup(), "gb peer learned startup");

    constexpr std::array<std::uint8_t, 12> wrong_ssrc{
        0x80,
        0x60,
        0x00,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x87,
        0x65,
        0x43,
        0x21,
    };
    wrong.send_to(boost::asio::buffer(wrong_ssrc), {boost::asio::ip::address_v4::loopback(), local_rtp_port});
    io.run_for(std::chrono::milliseconds(50));
    io.restart();

    const auto packets = make_ps_rtp(payload_type, ssrc);
    send_packets(expected, local_rtp_port, packets);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!streams.find("live/gb-peer-learned") && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(streams.find("live/gb-peer-learned") != nullptr, "gb peer learned ignores invalid first source and accepts first valid source");

    session->shutdown();
    io.run();
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::registry::init();
    try
    {
        media_server::test_ps_fixture_creates_stream();
        std::cout << "[pass] ps_fixture_creates_stream\n";
        media_server::test_output_same_codec_config_version_continues_ps_stream();
        std::cout << "[pass] output_same_codec_config_version_continues_ps_stream\n";
        media_server::test_signaled_rtp_peer_is_pinned_before_first_packet();
        std::cout << "[pass] signaled_rtp_peer_is_pinned_before_first_packet\n";
        media_server::test_first_valid_rtp_packet_pins_peer_when_unsignaled();
        std::cout << "[pass] first_valid_rtp_packet_pins_peer_when_unsignaled\n";
        media_server::registry::destroy();
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
