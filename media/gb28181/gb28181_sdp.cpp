#include <memory>
#include <cstring>
#include <utility>
#include <charconv>

#include <boost/asio/ip/address.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "media/gb28181/gb28181_sdp.h"

extern "C"
{
#include "sdp.h"
#include "sdp-options.h"
#include "sdp-a-rtpmap.h"
#include "sdp-a-webrtc.h"
}

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

std::optional<std::uint8_t> find_ps_payload(sdp_t* sdp)
{
    int formats[16]{};
    const auto format_count = sdp_media_formats(sdp, 0, formats, 16);
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
            sdp,
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

        if (lookup.payload == payload && lookup.rate == 90'000 && (boost::iequals(lookup.encoding, "PS") || boost::iequals(lookup.encoding, "MP2P")))
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
    return static_cast<std::uint8_t>(selected_payload);
}

std::optional<gb28181_description> parse_gb28181_common_description(sdp_t* sdp)
{
    if (sdp_media_count(sdp) != 1 || std::strcmp(sdp_media_type(sdp, 0), "video") != 0)
    {
        return std::nullopt;
    }

    int ports[2]{};
    if (sdp_media_port(sdp, 0, ports, 2) != 1 || ports[0] <= 0 || ports[0] > 65'535)
    {
        return std::nullopt;
    }

    char address_text[64]{};
    if (sdp_media_get_connection_address(sdp, 0, address_text, sizeof(address_text)) != 0)
    {
        return std::nullopt;
    }
    boost::system::error_code address_error;
    auto address = boost::asio::ip::make_address(address_text, address_error);
    if (address_error)
    {
        return std::nullopt;
    }

    const auto payload_type = find_ps_payload(sdp);
    if (!payload_type)
    {
        return std::nullopt;
    }

    auto ssrc = parse_ssrc(sdp_media_attribute_find(sdp, 0, "ssrc"));
    if (!ssrc)
    {
        ssrc = parse_ssrc(sdp_attribute_find(sdp, "ssrc"));
    }
    if (!ssrc)
    {
        return std::nullopt;
    }

    return gb28181_description{
        .address = std::move(address),
        .rtp_port = static_cast<std::uint16_t>(ports[0]),
        .payload_type = *payload_type,
        .ssrc = *ssrc,
    };
}

std::optional<gb28181_description> parse_gb28181_tcp_transport(sdp_t* sdp, gb28181_description description)
{
    const auto* setup_text = sdp_media_attribute_find(sdp, 0, "setup");
    if (setup_text == nullptr)
    {
        setup_text = sdp_attribute_find(sdp, "setup");
    }
    const auto setup = sdp_option_setup_from(setup_text);
    if (setup != SDP_A_SETUP_ACTIVE && setup != SDP_A_SETUP_PASSIVE)
    {
        return std::nullopt;
    }

    auto* connection = sdp_media_attribute_find(sdp, 0, "connection");
    if (connection == nullptr)
    {
        connection = sdp_attribute_find(sdp, "connection");
    }
    if (connection != nullptr && !boost::iequals(connection, "new"))
    {
        return std::nullopt;
    }
    if (setup == SDP_A_SETUP_ACTIVE && description.address.is_unspecified())
    {
        return std::nullopt;
    }

    description.transport = setup == SDP_A_SETUP_ACTIVE ? gb28181_transport::tcp_active : gb28181_transport::tcp_passive;
    return description;
}

std::optional<gb28181_description> parse_gb28181_udp_transport(sdp_t* sdp, gb28181_description description)
{
    if (sdp_media_attribute_find(sdp, 0, "rtcp-mux") != nullptr)
    {
        return std::nullopt;
    }

    std::uint16_t rtcp_port{};
    if (const auto* rtcp = sdp_media_attribute_find(sdp, 0, "rtcp"))
    {
        sdp_address_t rtcp_address{};
        if (sdp_a_rtcp(rtcp, static_cast<int>(std::strlen(rtcp)), &rtcp_address) != 0 || rtcp_address.port[0] <= 0 || rtcp_address.port[0] > 65'535)
        {
            return std::nullopt;
        }
        rtcp_port = static_cast<std::uint16_t>(rtcp_address.port[0]);
    }
    else
    {
        if (description.rtp_port == 65'535)
        {
            return std::nullopt;
        }
        rtcp_port = static_cast<std::uint16_t>(description.rtp_port + 1);
    }
    if (rtcp_port == description.rtp_port)
    {
        return std::nullopt;
    }

    description.rtcp_port = rtcp_port;
    return description;
}

}    // namespace

std::optional<gb28181_description> parse_gb28181_sdp(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    sdp_ptr sdp(sdp_parse(text.data(), static_cast<int>(text.size())), &sdp_destroy);
    if (!sdp)
    {
        return std::nullopt;
    }

    auto description = parse_gb28181_common_description(sdp.get());
    if (!description)
    {
        return std::nullopt;
    }

    const auto proto = sdp_option_proto_from(sdp_media_proto(sdp.get(), 0));
    if (proto != SDP_M_PROTO_RTP_AVP && proto != SDP_M_PROTO_RTP_AVP_TCP)
    {
        return std::nullopt;
    }
    if (proto == SDP_M_PROTO_RTP_AVP_TCP)
    {
        return parse_gb28181_tcp_transport(sdp.get(), std::move(*description));
    }
    return parse_gb28181_udp_transport(sdp.get(), std::move(*description));
}

}    // namespace media_server
