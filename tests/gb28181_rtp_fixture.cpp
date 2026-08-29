#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

extern "C"
{
#include "rtp-profile.h"
#include "rtsp-muxer.h"
}

namespace
{

struct sender_context
{
    boost::asio::ip::udp::socket* socket{};
    boost::asio::ip::udp::endpoint target;
    boost::system::error_code error;
};

int send_packet(void* param, int, const void* packet, int bytes, std::uint32_t, int)
{
    auto& sender = *static_cast<sender_context*>(param);
    sender.socket->send_to(boost::asio::buffer(packet, static_cast<std::size_t>(bytes)), sender.target, 0, sender.error);
    return sender.error ? -1 : 0;
}

}    // namespace

int main(int argc, char** argv)
{
    if (argc != 6)
    {
        std::cerr << "usage: gb28181_rtp_fixture host port payload_type ssrc annexb.h264\n";
        return 1;
    }

    const auto parse_unsigned = [](std::string_view text, std::uint64_t maximum) -> std::optional<std::uint64_t>
    {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > maximum)
        {
            return std::nullopt;
        }
        return value;
    };
    const auto port = parse_unsigned(argv[2], std::numeric_limits<std::uint16_t>::max());
    const auto payload_type = parse_unsigned(argv[3], 127);
    const auto ssrc = parse_unsigned(argv[4], std::numeric_limits<std::uint32_t>::max());
    if (!port || !payload_type || !ssrc)
    {
        std::cerr << "invalid RTP parameters\n";
        return 1;
    }

    std::ifstream input(argv[5], std::ios::binary | std::ios::ate);
    if (!input)
    {
        std::cerr << "open H264 input failed\n";
        return 1;
    }
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
    {
        std::cerr << "invalid H264 input size\n";
        return 1;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
    {
        std::cerr << "read H264 input failed\n";
        return 1;
    }

    std::vector<std::pair<std::size_t, std::size_t>> nals;
    for (std::size_t offset = 0; offset + 4U <= data.size();)
    {
        std::size_t prefix{};
        if (offset + 5U <= data.size() && data[offset] == 0 && data[offset + 1U] == 0 && data[offset + 2U] == 0 && data[offset + 3U] == 1)
        {
            prefix = 4;
        }
        else if (data[offset] == 0 && data[offset + 1U] == 0 && data[offset + 2U] == 1)
        {
            prefix = 3;
        }
        if (prefix == 0)
        {
            ++offset;
            continue;
        }
        nals.emplace_back(offset, offset + prefix);
        offset += prefix + 1U;
    }
    std::vector<std::size_t> access_units;
    std::vector<std::uint8_t> config;
    for (std::size_t index = 0; index < nals.size(); ++index)
    {
        const auto [start, header] = nals[index];
        const auto type = static_cast<std::uint8_t>(data[header] & 0x1fU);
        const auto end = index + 1U < nals.size() ? nals[index + 1U].first : data.size();
        if (type == 7 || type == 8)
        {
            config.insert(config.end(), data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(end));
        }
        if (type == 9)
        {
            access_units.push_back(start);
        }
    }
    if (config.empty() || access_units.empty() || access_units.front() != 0)
    {
        std::cerr << "H264 input requires SPS/PPS and AUD-delimited access units\n";
        return 1;
    }

    boost::asio::io_context io;
    boost::system::error_code network_error;
    const auto address = boost::asio::ip::make_address(argv[1], network_error);
    if (network_error || !address.is_v4())
    {
        std::cerr << "invalid IPv4 target\n";
        return 1;
    }
    boost::asio::ip::udp::socket socket(io);
    socket.open(boost::asio::ip::udp::v4(), network_error);
    if (network_error)
    {
        std::cerr << "open UDP socket failed\n";
        return 1;
    }
    sender_context sender{
        .socket = &socket,
        .target = {address, static_cast<std::uint16_t>(*port)},
        .error = {},
    };
    auto* muxer = rtsp_muxer_create(&send_packet, &sender);
    if (muxer == nullptr)
    {
        std::cerr << "create RTP/PS muxer failed\n";
        return 1;
    }
    const auto payload = rtsp_muxer_add_payload(muxer,
                                                "RTP/AVP",
                                                90'000,
                                                static_cast<int>(*payload_type),
                                                "PS",
                                                1,
                                                static_cast<std::uint32_t>(*ssrc),
                                                0,
                                                config.data(),
                                                static_cast<int>(config.size()));
    const auto media = payload < 0 ? -1 : rtsp_muxer_add_media(
                                              muxer, payload, RTP_PAYLOAD_H264, config.data(), static_cast<int>(config.size()));
    if (media < 0)
    {
        static_cast<void>(rtsp_muxer_destroy(muxer));
        std::cerr << "configure RTP/PS muxer failed\n";
        return 1;
    }

    auto next_frame = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < access_units.size(); ++index)
    {
        const auto start = access_units[index];
        const auto end = index + 1U < access_units.size() ? access_units[index + 1U] : data.size();
        bool keyframe = false;
        for (const auto [nal_start, header] : nals)
        {
            if (nal_start >= end)
            {
                break;
            }
            if (nal_start >= start && (data[header] & 0x1fU) == 5)
            {
                keyframe = true;
                break;
            }
        }
        const auto timestamp = static_cast<std::int64_t>(index * 40U);
        if (rtsp_muxer_input(muxer,
                             media,
                             timestamp,
                             timestamp,
                             data.data() + start,
                             static_cast<int>(end - start),
                             keyframe ? 1 : 0) != 0 ||
            sender.error)
        {
            static_cast<void>(rtsp_muxer_destroy(muxer));
            std::cerr << "send RTP/PS failed\n";
            return 1;
        }
        next_frame += std::chrono::milliseconds(40);
        std::this_thread::sleep_until(next_frame);
    }
    return rtsp_muxer_destroy(muxer) == 0 ? 0 : 1;
}
