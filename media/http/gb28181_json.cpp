#include <limits>
#include <utility>
#include <algorithm>
#include <initializer_list>

#include <boost/json.hpp>
#include <boost/asio/ip/address.hpp>

#include "media/http/gb28181_json.h"

namespace media_server
{
namespace
{

using json_object = boost::json::object;
using json_value = boost::json::value;

std::optional<json_object> parse_object(std::string_view body)
{
    boost::system::error_code error;
    auto value = boost::json::parse(body, error);
    if (error || !value.is_object())
    {
        return std::nullopt;
    }
    return value.as_object();
}

bool has_only_fields(const json_object& object, std::initializer_list<std::string_view> fields)
{
    for (const auto& [key, value] : object)
    {
        static_cast<void>(value);
        if (std::find(fields.begin(), fields.end(), std::string_view{key}) == fields.end())
        {
            return false;
        }
    }
    return true;
}

std::optional<std::string> required_string(const json_object& object, std::string_view key)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty())
    {
        return std::nullopt;
    }
    return std::string{value->as_string()};
}

std::optional<std::uint64_t> unsigned_value(const json_value& value, std::uint64_t maximum)
{
    if (value.is_uint64())
    {
        const auto result = value.as_uint64();
        return result <= maximum ? std::optional<std::uint64_t>{result} : std::nullopt;
    }
    if (value.is_int64())
    {
        const auto result = value.as_int64();
        if (result < 0)
        {
            return std::nullopt;
        }
        const auto unsigned_result = static_cast<std::uint64_t>(result);
        return unsigned_result <= maximum ? std::optional<std::uint64_t>{unsigned_result} : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::uint16_t> required_port(const json_object& object, std::string_view key)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    const auto parsed = unsigned_value(*value, std::numeric_limits<std::uint16_t>::max());
    if (!parsed || *parsed == 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

bool optional_port(const json_object& object, std::string_view key, std::optional<std::uint16_t>& result)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
    {
        return true;
    }
    const auto parsed = unsigned_value(*value, std::numeric_limits<std::uint16_t>::max());
    if (!parsed || *parsed == 0)
    {
        return false;
    }
    result = static_cast<std::uint16_t>(*parsed);
    return true;
}

std::optional<std::uint8_t> required_payload_type(const json_object& object)
{
    const auto* value = object.if_contains("payload_type");
    if (value == nullptr)
    {
        return std::nullopt;
    }
    const auto parsed = unsigned_value(*value, 127);
    if (!parsed)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*parsed);
}

std::optional<std::uint32_t> required_ssrc(const json_object& object)
{
    const auto* value = object.if_contains("ssrc");
    if (value == nullptr)
    {
        return std::nullopt;
    }
    const auto parsed = unsigned_value(*value, std::numeric_limits<std::uint32_t>::max());
    if (!parsed)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*parsed);
}

std::optional<gb28181_transport> required_transport(const json_object& object)
{
    const auto* value = object.if_contains("transport");
    if (value == nullptr || !value->is_string())
    {
        return std::nullopt;
    }
    const auto transport = value->as_string();
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
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty())
    {
        return std::nullopt;
    }
    boost::system::error_code error;
    auto address = boost::asio::ip::make_address(value->as_string(), error);
    if (error || address.is_unspecified())
    {
        return std::nullopt;
    }
    return address;
}

bool optional_bool(const json_object& object, std::string_view key, bool& result)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
    {
        return true;
    }
    if (!value->is_bool())
    {
        return false;
    }
    result = value->as_bool();
    return true;
}

}    // namespace

std::optional<gb28181_receiver_config> parse_gb28181_receiver_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object)
    {
        return std::nullopt;
    }

    auto stream_name = required_string(*object, "stream_name");
    auto transport = required_transport(*object);
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    if (!stream_name || !transport || !payload_type || !ssrc)
    {
        return std::nullopt;
    }

    gb28181_transport_config config;
    config.mode = *transport;
    config.payload_type = *payload_type;
    config.ssrc = *ssrc;

    switch (*transport)
    {
        case gb28181_transport::udp:
            if (!has_only_fields(*object, {"stream_name", "transport", "payload_type", "ssrc"}))
            {
                return std::nullopt;
            }
            break;

        case gb28181_transport::tcp_active:
        {
            if (!has_only_fields(*object, {"stream_name", "transport", "remote_address", "remote_port", "payload_type", "ssrc"}))
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
            if (!has_only_fields(*object, {"stream_name", "transport", "listen_port", "payload_type", "ssrc"}))
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

    return gb28181_receiver_config{.stream_name = std::move(*stream_name), .transport = std::move(config)};
}

std::optional<gb28181_sender_config> parse_gb28181_sender_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object)
    {
        return std::nullopt;
    }

    auto stream_name = required_string(*object, "stream_name");
    auto sender_id = required_string(*object, "sender_id");
    auto transport = required_transport(*object);
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    if (!stream_name || !sender_id || !transport || !payload_type || !ssrc)
    {
        return std::nullopt;
    }

    gb28181_transport_config config;
    config.mode = *transport;
    config.payload_type = *payload_type;
    config.ssrc = *ssrc;
    bool rtcp_enabled = false;

    switch (*transport)
    {
        case gb28181_transport::udp:
        {
            if (!has_only_fields(*object,
                                 {"stream_name",
                                  "sender_id",
                                  "transport",
                                  "remote_address",
                                  "remote_rtp_port",
                                  "remote_rtcp_port",
                                  "payload_type",
                                  "ssrc",
                                  "rtcp_enabled"}))
            {
                return std::nullopt;
            }
            auto remote_address = required_address(*object, "remote_address");
            auto remote_rtp_port = required_port(*object, "remote_rtp_port");
            std::optional<std::uint16_t> remote_rtcp_port;
            if (!remote_address || !remote_rtp_port || !optional_port(*object, "remote_rtcp_port", remote_rtcp_port) ||
                !optional_bool(*object, "rtcp_enabled", rtcp_enabled) || rtcp_enabled != remote_rtcp_port.has_value() ||
                (remote_rtcp_port && *remote_rtcp_port == *remote_rtp_port))
            {
                return std::nullopt;
            }
            config.remote_address = std::move(*remote_address);
            config.remote_rtp_port = *remote_rtp_port;
            config.remote_rtcp_port = remote_rtcp_port.value_or(0);
            break;
        }

        case gb28181_transport::tcp_active:
        {
            if (!has_only_fields(*object,
                                 {"stream_name", "sender_id", "transport", "remote_address", "remote_port", "payload_type", "ssrc"}))
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
            if (!has_only_fields(*object, {"stream_name", "sender_id", "transport", "listen_port", "payload_type", "ssrc"}))
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

    return gb28181_sender_config{
        .stream_name = std::move(*stream_name),
        .sender_id = std::move(*sender_id),
        .transport = std::move(config),
        .rtcp_enabled = rtcp_enabled};
}

std::optional<std::string> parse_gb28181_receiver_delete(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_name"}))
    {
        return std::nullopt;
    }
    return required_string(*object, "stream_name");
}

std::optional<std::pair<std::string, std::string>> parse_gb28181_sender_delete(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_name", "sender_id"}))
    {
        return std::nullopt;
    }
    auto stream_name = required_string(*object, "stream_name");
    auto sender_id = required_string(*object, "sender_id");
    if (!stream_name || !sender_id)
    {
        return std::nullopt;
    }
    return std::pair{std::move(*stream_name), std::move(*sender_id)};
}

}    // namespace media_server
