#include <limits>
#include <utility>
#include <algorithm>
#include <initializer_list>

#include <simdjson.h>
#include <boost/asio/ip/address.hpp>

#include "media/http/gb28181_json.h"

namespace media_server
{
namespace
{

using json_object = simdjson::dom::object;

std::optional<json_object> parse_object(simdjson::dom::parser& parser, std::string_view body)
{
    json_object object;
    if (parser.parse(body.data(), body.size()).get(object) != simdjson::SUCCESS)
    {
        return std::nullopt;
    }
    return object;
}

bool has_only_fields(const json_object& object, std::initializer_list<std::string_view> fields)
{
    std::uint32_t seen = 0;
    for (const auto field : object)
    {
        const auto position = std::find(fields.begin(), fields.end(), field.key);
        if (position == fields.end())
        {
            return false;
        }
        const auto bit = 1U << static_cast<unsigned int>(position - fields.begin());
        if ((seen & bit) != 0)
        {
            return false;
        }
        seen |= bit;
    }
    return true;
}

std::optional<std::string> required_string(const json_object& object, std::string_view key)
{
    std::string_view value;
    if (object.at_key(key).get_string().get(value) != simdjson::SUCCESS || value.empty())
    {
        return std::nullopt;
    }
    return std::string{value};
}

std::optional<std::uint64_t> unsigned_value(simdjson::simdjson_result<simdjson::dom::element> value, std::uint64_t maximum)
{
    std::uint64_t result = 0;
    if (value.get_uint64().get(result) != simdjson::SUCCESS || result > maximum)
    {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint16_t> required_port(const json_object& object, std::string_view key)
{
    const auto parsed = unsigned_value(object.at_key(key), std::numeric_limits<std::uint16_t>::max());
    if (!parsed || *parsed == 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

bool optional_port(const json_object& object, std::string_view key, std::optional<std::uint16_t>& result)
{
    const auto value = object.at_key(key);
    if (value.error() == simdjson::NO_SUCH_FIELD)
    {
        return true;
    }
    const auto parsed = unsigned_value(value, std::numeric_limits<std::uint16_t>::max());
    if (!parsed || *parsed == 0)
    {
        return false;
    }
    result = static_cast<std::uint16_t>(*parsed);
    return true;
}

std::optional<std::uint8_t> required_payload_type(const json_object& object)
{
    const auto parsed = unsigned_value(object.at_key("payload_type"), 127);
    if (!parsed)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*parsed);
}

std::optional<std::uint32_t> required_ssrc(const json_object& object)
{
    const auto parsed = unsigned_value(object.at_key("ssrc"), std::numeric_limits<std::uint32_t>::max());
    if (!parsed)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*parsed);
}

std::optional<gb28181_transport> required_transport(const json_object& object)
{
    std::string_view transport;
    if (object.at_key("transport").get_string().get(transport) != simdjson::SUCCESS)
    {
        return std::nullopt;
    }
    if (transport == "udp")
    {
        return gb28181_transport::udp;
    }
    if (transport == "tcp_active")
    {
        return gb28181_transport::tcp_active;
    }
    if (transport == "tcp_passive")
    {
        return gb28181_transport::tcp_passive;
    }
    return std::nullopt;
}

std::optional<boost::asio::ip::address> required_address(const json_object& object, std::string_view key)
{
    std::string_view value;
    if (object.at_key(key).get_string().get(value) != simdjson::SUCCESS || value.empty())
    {
        return std::nullopt;
    }
    boost::system::error_code error;
    auto address = boost::asio::ip::make_address(value, error);
    if (error || address.is_unspecified())
    {
        return std::nullopt;
    }
    return address;
}

}    // namespace

std::optional<gb28181_receiver_config> parse_gb28181_receiver_config(std::string_view body, gb28181_receiver_kind kind)
{
    simdjson::dom::parser parser;
    const auto object = parse_object(parser, body);
    if (!object)
    {
        return std::nullopt;
    }

    auto stream_id = required_string(*object, "stream_id");
    auto transport = required_transport(*object);
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    if (!stream_id || !transport || !payload_type || !ssrc)
    {
        return std::nullopt;
    }

    gb28181_receiver_config config{
        .stream_id = std::move(*stream_id),
        .transport = *transport,
        .payload_type = *payload_type,
        .ssrc = *ssrc,
    };
    if (kind == gb28181_receiver_kind::playback)
    {
        if (*transport != gb28181_transport::tcp_passive || !has_only_fields(*object, {"stream_id", "transport", "payload_type", "ssrc"}))
        {
            return std::nullopt;
        }
        return config;
    }
    switch (*transport)
    {
        case gb28181_transport::udp:
            if (!has_only_fields(*object, {"stream_id", "transport", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            break;

        case gb28181_transport::tcp_active:
        {
            if (!has_only_fields(*object, {"stream_id", "transport", "remote_address", "remote_port", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            auto remote_address = required_address(*object, "remote_address");
            auto remote_port = required_port(*object, "remote_port");
            if (!remote_address || !remote_port)
            {
                return std::nullopt;
            }
            config.remote_address = std::move(*remote_address);
            config.remote_port = *remote_port;
            break;
        }

        case gb28181_transport::tcp_passive:
        {
            if (!has_only_fields(*object, {"stream_id", "transport", "listen_port", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            auto listen_port = required_port(*object, "listen_port");
            if (!listen_port)
            {
                return std::nullopt;
            }
            config.listen_port = *listen_port;
            break;
        }
    }

    return config;
}

std::optional<gb28181_sender_config> parse_gb28181_sender_config(std::string_view body)
{
    simdjson::dom::parser parser;
    const auto object = parse_object(parser, body);
    if (!object)
    {
        return std::nullopt;
    }

    auto stream_id = required_string(*object, "stream_id");
    auto sender_id = required_string(*object, "sender_id");
    auto transport = required_transport(*object);
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    if (!stream_id || !sender_id || !transport || !payload_type || !ssrc)
    {
        return std::nullopt;
    }

    gb28181_sender_config config{
        .stream_id = std::move(*stream_id),
        .sender_id = std::move(*sender_id),
        .transport = *transport,
        .payload_type = *payload_type,
        .ssrc = *ssrc,
    };
    switch (*transport)
    {
        case gb28181_transport::udp:
        {
            if (!has_only_fields(
                    *object,
                    {"stream_id", "sender_id", "transport", "remote_address", "remote_rtp_port", "remote_rtcp_port", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            auto remote_address = required_address(*object, "remote_address");
            auto remote_rtp_port = required_port(*object, "remote_rtp_port");
            std::optional<std::uint16_t> remote_rtcp_port;
            if (!remote_address || !remote_rtp_port || !optional_port(*object, "remote_rtcp_port", remote_rtcp_port) ||
                (remote_rtcp_port && *remote_rtcp_port == *remote_rtp_port))
            {
                return std::nullopt;
            }
            config.remote_address = std::move(*remote_address);
            config.remote_rtp_port = *remote_rtp_port;
            config.remote_rtcp_port = std::move(remote_rtcp_port);
            break;
        }

        case gb28181_transport::tcp_active:
        {
            if (!has_only_fields(*object, {"stream_id", "sender_id", "transport", "remote_address", "remote_port", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            auto remote_address = required_address(*object, "remote_address");
            auto remote_port = required_port(*object, "remote_port");
            if (!remote_address || !remote_port)
            {
                return std::nullopt;
            }
            config.remote_address = std::move(*remote_address);
            config.remote_port = *remote_port;
            break;
        }

        case gb28181_transport::tcp_passive:
        {
            if (!has_only_fields(*object, {"stream_id", "sender_id", "transport", "listen_port", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            auto listen_port = required_port(*object, "listen_port");
            if (!listen_port)
            {
                return std::nullopt;
            }
            config.listen_port = *listen_port;
            break;
        }
    }

    return config;
}

std::optional<std::pair<std::string, std::uint32_t>> parse_gb28181_receiver_update(std::string_view body)
{
    simdjson::dom::parser parser;
    const auto object = parse_object(parser, body);
    if (!object || !has_only_fields(*object, {"stream_id", "ssrc"}))
    {
        return std::nullopt;
    }
    auto stream_id = required_string(*object, "stream_id");
    const auto ssrc = required_ssrc(*object);
    if (!stream_id || !ssrc)
    {
        return std::nullopt;
    }
    return std::pair{std::move(*stream_id), *ssrc};
}

std::optional<gb28181_sender_identity> parse_gb28181_sender_delete(std::string_view body)
{
    simdjson::dom::parser parser;
    const auto object = parse_object(parser, body);
    if (!object || !has_only_fields(*object, {"stream_id", "sender_id"}))
    {
        return std::nullopt;
    }
    auto stream_id = required_string(*object, "stream_id");
    auto sender_id = required_string(*object, "sender_id");
    if (!stream_id || !sender_id)
    {
        return std::nullopt;
    }
    return gb28181_sender_identity{.stream_id = std::move(*stream_id), .sender_id = std::move(*sender_id)};
}

}    // namespace media_server
