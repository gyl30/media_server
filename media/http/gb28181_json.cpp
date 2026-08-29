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
    if (error)
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

std::optional<gb28181_input_config> parse_gb28181_input_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_name", "transport", "address", "rtp_port", "payload_type", "ssrc"}))
    {
        return std::nullopt;
    }

    auto stream_name = required_string(*object, "stream_name");
    auto transport = required_transport(*object);
    auto address = required_address(*object, "address");
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    std::optional<std::uint16_t> rtp_port;
    if (!stream_name || !transport || !address || !payload_type || !ssrc || !optional_port(*object, "rtp_port", rtp_port))
    {
        return std::nullopt;
    }

    if (*transport == gb28181_transport::udp)
    {
        if (rtp_port)
        {
            return std::nullopt;
        }
    }
    else if (!rtp_port)
    {
        return std::nullopt;
    }

    if (*transport == gb28181_transport::tcp_active && address->is_unspecified())
    {
        return std::nullopt;
    }

    return gb28181_input_config{.stream_name = std::move(*stream_name),
                                .description = gb28181_description{.transport = *transport,
                                                                   .address = *address,
                                                                   .rtp_port = rtp_port.value_or(0),
                                                                   .rtcp_port = 0,
                                                                   .payload_type = *payload_type,
                                                                   .ssrc = *ssrc}};
}

std::optional<gb28181_output_config> parse_gb28181_output_config(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object ||
        !has_only_fields(*object, {"stream_name", "output_id", "transport", "address", "rtp_port", "rtcp_port", "payload_type", "ssrc", "rtcp"}))
    {
        return std::nullopt;
    }

    auto stream_name = required_string(*object, "stream_name");
    auto output_id = required_string(*object, "output_id");
    auto transport = required_transport(*object);
    auto address = required_address(*object, "address");
    auto rtp_port = required_port(*object, "rtp_port");
    auto payload_type = required_payload_type(*object);
    auto ssrc = required_ssrc(*object);
    std::optional<std::uint16_t> rtcp_port;
    bool rtcp = false;
    if (!stream_name || !output_id || !transport || !address || !rtp_port || !payload_type || !ssrc ||
        !optional_port(*object, "rtcp_port", rtcp_port) || !optional_bool(*object, "rtcp", rtcp))
    {
        return std::nullopt;
    }
    if (*transport == gb28181_transport::udp)
    {
        if (!rtcp_port || *rtcp_port == *rtp_port || address->is_unspecified())
        {
            return std::nullopt;
        }
    }
    else if (rtcp_port || rtcp)
    {
        return std::nullopt;
    }

    if (*transport == gb28181_transport::tcp_active && address->is_unspecified())
    {
        return std::nullopt;
    }

    return gb28181_output_config{.stream_name = std::move(*stream_name),
                                 .output_id = std::move(*output_id),
                                 .description = gb28181_description{.transport = *transport,
                                                                    .address = *address,
                                                                    .rtp_port = *rtp_port,
                                                                    .rtcp_port = rtcp_port.value_or(0),
                                                                    .payload_type = *payload_type,
                                                                    .ssrc = *ssrc},
                                 .rtcp = rtcp};
}

std::optional<std::string> parse_gb28181_input_delete(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_name"}))
    {
        return std::nullopt;
    }
    return required_string(*object, "stream_name");
}

std::optional<std::pair<std::string, std::string>> parse_gb28181_output_delete(std::string_view body)
{
    const auto object = parse_object(body);
    if (!object || !has_only_fields(*object, {"stream_name", "output_id"}))
    {
        return std::nullopt;
    }
    auto stream_name = required_string(*object, "stream_name");
    auto output_id = required_string(*object, "output_id");
    if (!stream_name || !output_id)
    {
        return std::nullopt;
    }
    return std::pair{std::move(*stream_name), std::move(*output_id)};
}

}    // namespace media_server
