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
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"

extern "C"
{
#include "rtsp-muxer.h"
#include "rtp-packet.h"
#include "rtp-profile.h"
#include "mpeg-util.h"
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

std::vector<std::vector<std::uint8_t>> make_ps_rtp(std::uint8_t payload_type,
                                                       std::uint32_t ssrc,
                                                       std::uint16_t sequence = 1,
                                                       int video_codec = RTP_PAYLOAD_H264,
                                                       std::optional<int> audio_codec = std::nullopt)
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
    const auto payload = rtsp_muxer_add_payload(
        muxer, "RTP/AVP", 90'000, payload_type, "PS", sequence, ssrc, 0, config.data(), static_cast<int>(config.size()));
    require(payload >= 0, "gb peer ps payload");
    const auto video_media = rtsp_muxer_add_media(muxer, payload, video_codec, config.data(), static_cast<int>(config.size()));
    require(video_media >= 0, "gb peer ps video media");
    int audio_media = -1;
    if (audio_codec)
    {
        audio_media = rtsp_muxer_add_media(muxer, payload, *audio_codec, nullptr, 0);
        require(audio_media >= 0, "gb peer ps audio media");
    }

    auto frame = config;
    const std::array<std::uint8_t, 9> idr{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0};
    frame.insert(frame.end(), idr.begin(), idr.end());
    require(rtsp_muxer_input(muxer, video_media, 0, 0, frame.data(), static_cast<int>(frame.size()), 1) == 0, "gb peer ps first frame");
    if (audio_media >= 0)
    {
        const std::array<std::uint8_t, 160> audio_frame{};
        require(rtsp_muxer_input(muxer, audio_media, 20, 20, audio_frame.data(), static_cast<int>(audio_frame.size()), 0) == 0,
                "gb peer ps audio frame");
    }
    const std::array<std::uint8_t, 8> p_frame{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
    require(rtsp_muxer_input(muxer, video_media, 40, 40, p_frame.data(), static_cast<int>(p_frame.size()), 0) == 0,
            "gb peer ps second frame");
    require(rtsp_muxer_destroy(muxer) == 0 && !packets.empty(), "gb peer ps packets");
    return packets;
}

void set_psm_version(std::vector<std::vector<std::uint8_t>>& packets, std::uint8_t version)
{
    bool updated = false;
    for (auto& packet : packets)
    {
        for (std::size_t i = 12; i + 10 < packet.size(); ++i)
        {
            if (packet[i] != 0x00 || packet[i + 1] != 0x00 || packet[i + 2] != 0x01 || packet[i + 3] != 0xbc)
            {
                continue;
            }

            const auto length = static_cast<std::size_t>((static_cast<std::uint16_t>(packet[i + 4]) << 8U) | packet[i + 5]);
            const auto bytes = length + 6U;
            require(i + bytes <= packet.size() && bytes >= 10U, "gb peer psm fits packet");
            packet[i + 6] = static_cast<std::uint8_t>((packet[i + 6] & 0xe0U) | (version & 0x1fU));
            const auto crc = mpeg_crc32(0xffffffffU, packet.data() + i, static_cast<std::uint32_t>(bytes - 4U));
            packet[i + bytes - 4U] = static_cast<std::uint8_t>(crc & 0xffU);
            packet[i + bytes - 3U] = static_cast<std::uint8_t>((crc >> 8U) & 0xffU);
            packet[i + bytes - 2U] = static_cast<std::uint8_t>((crc >> 16U) & 0xffU);
            packet[i + bytes - 1U] = static_cast<std::uint8_t>((crc >> 24U) & 0xffU);
            updated = true;
        }
    }
    require(updated, "gb peer psm version updated");
}

std::uint16_t next_rtp_sequence(const std::vector<std::vector<std::uint8_t>>& packets)
{
    require(!packets.empty(), "gb peer rtp sequence packets");
    rtp_packet_t packet{};
    require(rtp_packet_deserialize(&packet, packets.back().data(), static_cast<int>(packets.back().size())) == 0,
            "gb peer rtp sequence packet");
    return static_cast<std::uint16_t>(packet.rtp.seq + 1U);
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
    worker_context worker;
    worker.release_work();
    auto& streams = media_server::registry::instance();
    streams.clear();
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    gb28181_input_media media(worker, "live/gb-peer-fixture", payload_type, ssrc);
    require(media.startup(), "gb peer fixture media startup");
    const auto packets = make_ps_rtp(payload_type, ssrc);
    for (const auto& packet : packets)
    {
        require(media.input_rtp(packet) == gb28181_rtp_input_result::accepted, "gb peer fixture packet accepted");
    }
    require(streams.find("live/gb-peer-fixture") != nullptr, "gb peer fixture creates stream");
    media.shutdown();
}

void test_input_video_codec_change_is_fatal()
{
    worker_context worker;
    worker.release_work();
    auto& streams = media_server::registry::instance();
    streams.clear();
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    gb28181_input_media media(worker, "live/gb-video-codec-change", payload_type, ssrc);
    require(media.startup(), "gb video codec change startup");

    const auto initial_packets = make_ps_rtp(payload_type, ssrc);
    for (const auto& packet : initial_packets)
    {
        require(media.input_rtp(packet) == gb28181_rtp_input_result::accepted, "gb video codec initial packet accepted");
    }
    const auto stream = streams.find("live/gb-video-codec-change");
    require(stream != nullptr && stream->tracks().front().codec == codec_id::h264, "gb video codec initial h264 track");

    auto changed_packets = make_ps_rtp(payload_type, ssrc, next_rtp_sequence(initial_packets), RTP_PAYLOAD_H265);
    set_psm_version(changed_packets, 2);
    auto result = gb28181_rtp_input_result::accepted;
    for (const auto& packet : changed_packets)
    {
        result = media.input_rtp(packet);
        if (result == gb28181_rtp_input_result::fatal)
        {
            break;
        }
    }
    require(result == gb28181_rtp_input_result::fatal, "gb video codec h264 to h265 is fatal");
    media.shutdown();
}

void test_input_audio_codec_change_is_fatal()
{
    worker_context worker;
    worker.release_work();
    auto& streams = media_server::registry::instance();
    streams.clear();
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    gb28181_input_media media(worker, "live/gb-audio-codec-change", payload_type, ssrc);
    require(media.startup(), "gb audio codec change startup");

    const auto initial_packets = make_ps_rtp(payload_type, ssrc, 1, RTP_PAYLOAD_H264, RTP_PAYLOAD_PCMA);
    for (const auto& packet : initial_packets)
    {
        require(media.input_rtp(packet) == gb28181_rtp_input_result::accepted, "gb audio codec initial packet accepted");
    }
    const auto stream = streams.find("live/gb-audio-codec-change");
    require(stream != nullptr && stream->tracks().size() == 2U && stream->tracks()[1].codec == codec_id::g711a,
            "gb audio codec initial g711a track");

    auto changed_packets = make_ps_rtp(payload_type, ssrc, next_rtp_sequence(initial_packets), RTP_PAYLOAD_H264, RTP_PAYLOAD_PCMU);
    set_psm_version(changed_packets, 3);
    auto result = gb28181_rtp_input_result::accepted;
    for (const auto& packet : changed_packets)
    {
        result = media.input_rtp(packet);
        if (result == gb28181_rtp_input_result::fatal)
        {
            break;
        }
    }
    require(result == gb28181_rtp_input_result::fatal, "gb audio codec g711a to g711u is fatal");
    media.shutdown();
}

void test_udp_session_fatal_codec_change_unregisters()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = media_server::registry::instance();
    streams.clear();
    boost::asio::ip::udp::socket sender(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    const std::string stream_name = "live/gb-fatal-codec-session";
    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_session>(worker, stream_name, description);
    require(streams.add_input_session(stream_name, session), "gb fatal codec session registry add");
    require(session->startup(), "gb fatal codec session startup");
    const auto local_ports = session->local_ports();
    require(local_ports.has_value(), "gb fatal codec session local ports");

    const auto initial_packets = make_ps_rtp(payload_type, ssrc);
    send_packets(sender, local_ports->first, initial_packets);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!streams.find(stream_name) && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(streams.find(stream_name) != nullptr, "gb fatal codec session initial stream ready");

    auto changed_packets = make_ps_rtp(payload_type, ssrc, next_rtp_sequence(initial_packets), RTP_PAYLOAD_H265);
    set_psm_version(changed_packets, 2);
    send_packets(sender, local_ports->first, changed_packets);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (streams.find(stream_name) && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(!streams.find(stream_name), "gb fatal codec session removes stream");
    require(!streams.take_input_session(stream_name), "gb fatal codec session unregisters owner");
    io.run();
}

void test_output_same_codec_config_version_continues_ps_stream()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
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

    gb28181_input_media receiver(worker, "live/gb-output-config-received", payload_type, ssrc);
    require(receiver.startup(), "gb output config receiver startup");

    std::size_t packet_count = 0;
    std::size_t end_count = 0;
    std::vector<std::uint8_t> ps_payload;
    auto output = std::make_shared<gb28181_output_media>(
        worker,
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

void test_rtcp_peer_learning_overrides_rtp_plus_one()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = media_server::registry::instance();
    streams.clear();
    boost::asio::ip::udp::socket sender(io);
    boost::asio::ip::udp::socket default_rtcp(io);
    boost::system::error_code bind_error;
    std::uint16_t sender_port = 42'000;
    for (; sender_port < 49'000; sender_port = static_cast<std::uint16_t>(sender_port + 2U))
    {
        sender.open(boost::asio::ip::udp::v4(), bind_error);
        if (!bind_error)
        {
            sender.bind({boost::asio::ip::address_v4::loopback(), sender_port}, bind_error);
        }
        default_rtcp.open(boost::asio::ip::udp::v4(), bind_error);
        if (!bind_error)
        {
            default_rtcp.bind({boost::asio::ip::address_v4::loopback(), static_cast<std::uint16_t>(sender_port + 1U)}, bind_error);
        }
        if (!bind_error)
        {
            break;
        }
        boost::system::error_code close_error;
        sender.close(close_error);
        default_rtcp.close(close_error);
    }
    require(sender.is_open() && default_rtcp.is_open() && !bind_error, "gb rtcp peer remote port pair");
    boost::asio::ip::udp::socket actual_rtcp(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket foreign_rtcp(
        io, {boost::asio::ip::make_address_v4("127.0.0.2"), 0});

    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_session>(worker, "live/gb-rtcp-peer", description);
    require(session->startup(), "gb rtcp peer startup");
    const auto local_ports = session->local_ports();
    require(local_ports.has_value(), "gb rtcp peer local ports");

    const auto packets = make_ps_rtp(payload_type, ssrc);
    send_packets(sender, local_ports->first, packets);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!streams.find("live/gb-rtcp-peer") && std::chrono::steady_clock::now() < deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(streams.find("live/gb-rtcp-peer") != nullptr, "gb rtcp peer accepts RTP source");

    auto report_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (default_rtcp.available() == 0 && std::chrono::steady_clock::now() < report_deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(default_rtcp.available() > 0, "gb rtcp peer defaults to RTP source port plus one");
    std::array<std::uint8_t, 1500> received{};
    boost::asio::ip::udp::endpoint received_from;
    default_rtcp.receive_from(boost::asio::buffer(received), received_from);

    constexpr std::array<std::uint8_t, 28> sender_report{
        0x80, 0xc8, 0x00, 0x06, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    foreign_rtcp.send_to(boost::asio::buffer(sender_report), {boost::asio::ip::address_v4::loopback(), local_ports->second});
    report_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (default_rtcp.available() == 0 && foreign_rtcp.available() == 0 && std::chrono::steady_clock::now() < report_deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(foreign_rtcp.available() == 0, "gb rtcp peer rejects source from another RTP address");
    require(default_rtcp.available() > 0, "gb rtcp peer keeps RTP plus one fallback after foreign report");
    default_rtcp.receive_from(boost::asio::buffer(received), received_from);

    actual_rtcp.send_to(boost::asio::buffer(sender_report), {boost::asio::ip::address_v4::loopback(), local_ports->second});

    report_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (actual_rtcp.available() == 0 && std::chrono::steady_clock::now() < report_deadline)
    {
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
    }
    require(actual_rtcp.available() > 0, "gb rtcp peer uses learned RTCP source");
    require(default_rtcp.available() == 0, "gb rtcp peer stops using default target after learning");

    session->shutdown();
    io.restart();
    io.run();
}

void test_first_valid_rtp_packet_pins_peer_when_unsignaled()
{
    worker_context worker;
    worker.release_work();
    auto& io = worker.io();
    auto& streams = media_server::registry::instance();
    streams.clear();
    boost::asio::ip::udp::socket expected(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket wrong(io, {boost::asio::ip::address_v4::loopback(), 0});
    constexpr std::uint8_t payload_type = 96;
    constexpr std::uint32_t ssrc = 0x12345678U;
    const gb28181_description description{
        .transport = gb28181_transport::udp,
        .address = boost::asio::ip::address_v4::loopback(),
        .payload_type = payload_type,
        .ssrc = ssrc,
    };
    auto session = std::make_shared<gb28181_udp_session>(worker, "live/gb-peer-learned", description);
    require(session->startup(), "gb peer learned startup");
    const auto local_ports = session->local_ports();
    require(local_ports.has_value(), "gb peer learned local ports");

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
    wrong.send_to(boost::asio::buffer(wrong_ssrc), {boost::asio::ip::address_v4::loopback(), local_ports->first});
    io.run_for(std::chrono::milliseconds(50));
    io.restart();

    const auto packets = make_ps_rtp(payload_type, ssrc);
    send_packets(expected, local_ports->first, packets);
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
    media_server::port_manager::init(32'200, 32'399);
    media_server::registry::init();
    try
    {
        media_server::test_ps_fixture_creates_stream();
        std::cout << "[pass] ps_fixture_creates_stream\n";
        media_server::test_input_video_codec_change_is_fatal();
        std::cout << "[pass] input_video_codec_change_is_fatal\n";
        media_server::test_input_audio_codec_change_is_fatal();
        std::cout << "[pass] input_audio_codec_change_is_fatal\n";
        media_server::test_udp_session_fatal_codec_change_unregisters();
        std::cout << "[pass] udp_session_fatal_codec_change_unregisters\n";
        media_server::test_output_same_codec_config_version_continues_ps_stream();
        std::cout << "[pass] output_same_codec_config_version_continues_ps_stream\n";
        media_server::test_rtcp_peer_learning_overrides_rtp_plus_one();
        std::cout << "[pass] rtcp_peer_learning_overrides_rtp_plus_one\n";
        media_server::test_first_valid_rtp_packet_pins_peer_when_unsignaled();
        std::cout << "[pass] first_valid_rtp_packet_pins_peer_when_unsignaled\n";
        media_server::registry::destroy();
        media_server::port_manager::destroy();
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
