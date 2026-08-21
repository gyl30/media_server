#include "media/gb28181/gb28181_sdp.h"

extern "C"
{
#include "sdp-a-rtpmap.h"
#include "sdp-a-webrtc.h"
#include "sdp.h"
}

#include <boost/algorithm/string/predicate.hpp>

#include <charconv>
#include <cstring>
#include <memory>
#include <utility>

namespace media_server
{
namespace
{

using sdp_ptr = std::unique_ptr<sdp_t, decltype(&sdp_destroy)>;

std::optional<std::uint32_t> parse_ssrc(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return std::nullopt;
    }

    std::uint32_t value{};
    const auto length = std::strlen(text);
    const auto [end, error] = std::from_chars(text, text + length, value);
    if (error != std::errc{} || end != text + length)
    {
        return std::nullopt;
    }
    return value;
}

}    // namespace

std::optional<gb28181_udp_description> parse_gb28181_udp_sdp(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    sdp_ptr sdp(sdp_parse(text.data(), static_cast<int>(text.size())), &sdp_destroy);
    if (!sdp || sdp_media_count(sdp.get()) != 1 || std::strcmp(sdp_media_type(sdp.get(), 0), "video") != 0 ||
        std::strcmp(sdp_media_proto(sdp.get(), 0), "RTP/AVP") != 0)
    {
        return std::nullopt;
    }

    int ports[2]{};
    if (sdp_media_port(sdp.get(), 0, ports, 2) != 1 || ports[0] <= 0 || ports[0] > 65'535)
    {
        return std::nullopt;
    }

    const auto addrtype = sdp_media_get_connection_addrtype(sdp.get(), 0);
    boost::asio::ip::address bind_address;
    if (addrtype == SDP_C_ADDRESS_IP4)
    {
        bind_address = boost::asio::ip::address_v4::any();
    }
    else if (addrtype == SDP_C_ADDRESS_IP6)
    {
        bind_address = boost::asio::ip::address_v6::any();
    }
    else
    {
        return std::nullopt;
    }

    int formats[16]{};
    const auto format_count = sdp_media_formats(sdp.get(), 0, formats, 16);
    if (format_count <= 0 || format_count > 16)
    {
        return std::nullopt;
    }

    int selected_payload = -1;
    for (int index = 0; index < format_count; ++index)
    {
        const auto payload = formats[index];
        if (payload < 0 || payload > 127)
        {
            continue;
        }

        struct rtpmap_lookup
        {
            int wanted{};
            int payload{-1};
            int rate{};
            char encoding[16]{};
        } lookup{.wanted = payload};
        sdp_media_attribute_list(
            sdp.get(),
            0,
            "rtpmap",
            [](void* param, const char*, const char* value)
            {
                auto& result = *static_cast<rtpmap_lookup*>(param);
                int parsed_payload{};
                int rate{};
                char encoding[16]{};
                char parameters[64]{};
                if (value != nullptr && sdp_a_rtpmap(value, &parsed_payload, encoding, &rate, parameters) == 0 && parsed_payload == result.wanted)
                {
                    result.payload = parsed_payload;
                    result.rate = rate;
                    std::memcpy(result.encoding, encoding, sizeof(result.encoding));
                }
            },
            &lookup);

        if (lookup.payload == payload && lookup.rate == 90'000 &&
            (boost::iequals(lookup.encoding, "PS") || boost::iequals(lookup.encoding, "MP2P")))
        {
            if (selected_payload != -1)
            {
                return std::nullopt;
            }
            selected_payload = payload;
        }
    }
    if (selected_payload < 0)
    {
        return std::nullopt;
    }

    auto ssrc = parse_ssrc(sdp_media_attribute_find(sdp.get(), 0, "ssrc"));
    if (!ssrc)
    {
        ssrc = parse_ssrc(sdp_attribute_find(sdp.get(), "ssrc"));
    }
    if (!ssrc)
    {
        return std::nullopt;
    }

    if (sdp_media_attribute_find(sdp.get(), 0, "rtcp-mux") != nullptr)
    {
        return std::nullopt;
    }

    std::uint16_t rtcp_port{};
    if (const auto* rtcp = sdp_media_attribute_find(sdp.get(), 0, "rtcp"))
    {
        sdp_address_t address{};
        if (sdp_a_rtcp(rtcp, static_cast<int>(std::strlen(rtcp)), &address) != 0 || address.port[0] <= 0 || address.port[0] > 65'535)
        {
            return std::nullopt;
        }
        rtcp_port = static_cast<std::uint16_t>(address.port[0]);
    }
    else
    {
        if (ports[0] == 65'535)
        {
            return std::nullopt;
        }
        rtcp_port = static_cast<std::uint16_t>(ports[0] + 1);
    }
    if (rtcp_port == static_cast<std::uint16_t>(ports[0]))
    {
        return std::nullopt;
    }

    return gb28181_udp_description{
        .bind_address = std::move(bind_address),
        .rtp_port = static_cast<std::uint16_t>(ports[0]),
        .rtcp_port = rtcp_port,
        .payload_type = static_cast<std::uint8_t>(selected_payload),
        .ssrc = *ssrc,
    };
}

}    // namespace media_server
