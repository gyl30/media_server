#include <array>
#include <limits>
#include <vector>
#include <cstddef>
#include <algorithm>

#include <boost/crc.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>
#include <openssl/crypto.h>

#include "media/webrtc/stun_message.h"

namespace media_server
{
namespace
{

constexpr std::size_t stun_header_size = 20;
constexpr std::uint32_t stun_magic_cookie = 0x2112a442U;
constexpr std::uint16_t stun_binding_request_type = 0x0001;
constexpr std::uint16_t stun_binding_success_response_type = 0x0101;
constexpr std::uint16_t stun_binding_error_response_type = 0x0111;
constexpr std::uint16_t stun_attribute_username = 0x0006;
constexpr std::uint16_t stun_attribute_message_integrity = 0x0008;
constexpr std::uint16_t stun_attribute_error_code = 0x0009;
constexpr std::uint16_t stun_attribute_unknown_attributes = 0x000a;
constexpr std::uint16_t stun_attribute_xor_mapped_address = 0x0020;
constexpr std::uint16_t stun_attribute_priority = 0x0024;
constexpr std::uint16_t stun_attribute_use_candidate = 0x0025;
constexpr std::uint16_t stun_attribute_fingerprint = 0x8028;
constexpr std::uint16_t stun_attribute_ice_controlled = 0x8029;
constexpr std::uint16_t stun_attribute_ice_controlling = 0x802a;
constexpr std::size_t stun_message_integrity_size = 20;
constexpr std::uint32_t stun_fingerprint_xor = 0x5354554eU;

std::uint16_t read_u16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) | (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

void write_u16(std::uint8_t* data, std::uint16_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 8U);
    data[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void set_message_length(std::vector<std::uint8_t>& packet, std::size_t body_size)
{
    if (body_size <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
    {
        write_u16(packet.data() + 2, static_cast<std::uint16_t>(body_size));
    }
}

void append_attribute(std::vector<std::uint8_t>& packet, std::uint16_t type, std::span<const std::uint8_t> value)
{
    append_u16(packet, type);
    append_u16(packet, static_cast<std::uint16_t>(value.size()));
    packet.insert(packet.end(), value.begin(), value.end());
    while ((packet.size() % 4U) != 0U)
    {
        packet.push_back(0);
    }
}

std::optional<std::array<std::uint8_t, stun_message_integrity_size>> hmac_sha1(std::string_view password, std::span<const std::uint8_t> data)
{
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }

    std::array<std::uint8_t, stun_message_integrity_size> digest{};
    unsigned int digest_size = 0;
    const auto* result = HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()), data.data(), data.size(), digest.data(), &digest_size);
    if (result == nullptr || digest_size != digest.size())
    {
        return std::nullopt;
    }
    return digest;
}

std::uint32_t fingerprint(std::span<const std::uint8_t> data)
{
    boost::crc_32_type crc;
    crc.process_bytes(data.data(), data.size());
    return crc.checksum() ^ stun_fingerprint_xor;
}

bool verify_message_integrity(std::span<const std::uint8_t> packet, std::size_t attribute_offset, std::string_view password)
{
    if (attribute_offset < stun_header_size || attribute_offset + 24U > packet.size())
    {
        return false;
    }

    std::vector<std::uint8_t> input(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(attribute_offset));
    const auto body_size = attribute_offset - stun_header_size + 24U;
    if (body_size > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
    {
        return false;
    }
    set_message_length(input, body_size);

    const auto digest = hmac_sha1(password, input);
    return digest.has_value() && CRYPTO_memcmp(digest->data(), packet.data() + attribute_offset + 4U, digest->size()) == 0;
}

bool verify_fingerprint(std::span<const std::uint8_t> packet, std::size_t attribute_offset)
{
    if (attribute_offset < stun_header_size || attribute_offset + 8U > packet.size())
    {
        return false;
    }
    return fingerprint(packet.first(attribute_offset)) == read_u32(packet.data() + attribute_offset + 4U);
}

std::vector<std::uint8_t> make_header(std::uint16_t type, const std::array<std::uint8_t, 12>& transaction_id)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(80);
    append_u16(packet, type);
    append_u16(packet, 0);
    append_u32(packet, stun_magic_cookie);
    packet.insert(packet.end(), transaction_id.begin(), transaction_id.end());
    return packet;
}

bool append_integrity_and_fingerprint(std::vector<std::uint8_t>& packet, std::string_view password)
{
    const auto body_before_integrity = packet.size() - stun_header_size;
    set_message_length(packet, body_before_integrity + 24U);
    const auto digest = hmac_sha1(password, packet);
    if (!digest)
    {
        return false;
    }
    append_attribute(packet, stun_attribute_message_integrity, *digest);

    const auto body_before_fingerprint = packet.size() - stun_header_size;
    set_message_length(packet, body_before_fingerprint + 8U);
    const auto fingerprint_value = fingerprint(packet);
    const std::array<std::uint8_t, 4> fingerprint_bytes{
        static_cast<std::uint8_t>(fingerprint_value >> 24U),
        static_cast<std::uint8_t>((fingerprint_value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((fingerprint_value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(fingerprint_value & 0xffU),
    };
    append_attribute(packet, stun_attribute_fingerprint, fingerprint_bytes);
    return true;
}

std::vector<std::uint8_t> xor_mapped_address(const boost::asio::ip::udp::endpoint& endpoint, const std::array<std::uint8_t, 12>& transaction_id)
{
    std::vector<std::uint8_t> value;
    value.reserve(endpoint.address().is_v4() ? 8U : 20U);
    value.push_back(0);
    value.push_back(endpoint.address().is_v4() ? 0x01U : 0x02U);
    append_u16(value, static_cast<std::uint16_t>(endpoint.port() ^ static_cast<std::uint16_t>(stun_magic_cookie >> 16U)));

    constexpr std::array<std::uint8_t, 4> cookie_bytes{0x21, 0x12, 0xa4, 0x42};
    if (endpoint.address().is_v4())
    {
        const auto address = endpoint.address().to_v4().to_bytes();
        for (std::size_t index = 0; index < address.size(); ++index)
        {
            value.push_back(static_cast<std::uint8_t>(address[index] ^ cookie_bytes[index]));
        }
        return value;
    }

    const auto address = endpoint.address().to_v6().to_bytes();
    for (std::size_t index = 0; index < address.size(); ++index)
    {
        const auto mask = index < cookie_bytes.size() ? cookie_bytes[index] : transaction_id[index - cookie_bytes.size()];
        value.push_back(static_cast<std::uint8_t>(address[index] ^ mask));
    }
    return value;
}

struct stun_binding_attributes
{
    std::optional<std::string_view> username;
    std::optional<std::size_t> message_integrity_offset;
    std::optional<std::size_t> fingerprint_offset;
    std::vector<std::uint16_t> unknown_required_attributes;
    bool priority{};
    bool ice_controlling{};
    bool ice_controlled{};
    bool use_candidate{};
};

std::optional<stun_binding_attributes> scan_stun_attributes(std::span<const std::uint8_t> packet)
{
    stun_binding_attributes result;
    std::size_t offset = stun_header_size;
    while (offset + 4U <= packet.size())
    {
        const auto type = read_u16(packet.data() + offset);
        const auto length = static_cast<std::size_t>(read_u16(packet.data() + offset + 2U));
        const auto value_offset = offset + 4U;
        spdlog::trace("stun attribute type 0x{:04x} length {} offset {}", type, length, offset);

        if (value_offset + length > packet.size())
        {
            spdlog::debug("stun reject attribute overflow type 0x{:04x} length {}", type, length);
            return std::nullopt;
        }

        if (result.message_integrity_offset.has_value() && type != stun_attribute_fingerprint)
        {
            offset = value_offset + length;
            offset += (4U - (offset % 4U)) % 4U;
            continue;
        }

        if (type == stun_attribute_username)
        {
            if (result.username.has_value())
            {
                spdlog::debug("stun reject invalid username position");
                return std::nullopt;
            }
            result.username = std::string_view(reinterpret_cast<const char*>(packet.data() + value_offset), length);
            spdlog::trace("stun username {}", *result.username);
        }
        else if (type == stun_attribute_message_integrity)
        {
            if (length != stun_message_integrity_size || result.message_integrity_offset.has_value())
            {
                spdlog::debug("stun reject invalid message integrity attribute length {}", length);
                return std::nullopt;
            }
            result.message_integrity_offset = offset;
        }
        else if (type == stun_attribute_priority)
        {
            if (length != 4U || result.priority)
            {
                spdlog::debug("stun reject invalid priority attribute length {}", length);
                return std::nullopt;
            }
            result.priority = true;
        }
        else if (type == stun_attribute_use_candidate)
        {
            if (length != 0 || result.use_candidate)
            {
                spdlog::debug("stun reject invalid use candidate attribute length {}", length);
                return std::nullopt;
            }
            result.use_candidate = true;
        }
        else if (type == stun_attribute_fingerprint)
        {
            if (length != 4U || result.fingerprint_offset.has_value())
            {
                spdlog::debug("stun reject invalid fingerprint attribute length {}", length);
                return std::nullopt;
            }
            result.fingerprint_offset = offset;
        }
        else if (type == stun_attribute_ice_controlling)
        {
            if (length != 8U || result.ice_controlling)
            {
                spdlog::debug("stun reject invalid ice controlling attribute length {}", length);
                return std::nullopt;
            }
            result.ice_controlling = true;
        }
        else if (type == stun_attribute_ice_controlled)
        {
            if (length != 8U || result.ice_controlled)
            {
                spdlog::debug("stun reject invalid ice controlled attribute length {}", length);
                return std::nullopt;
            }
            result.ice_controlled = true;
        }
        else if (type < 0x8000U && !result.message_integrity_offset.has_value() && type != stun_attribute_error_code &&
                 type != stun_attribute_unknown_attributes && type != stun_attribute_xor_mapped_address)
        {
            result.unknown_required_attributes.push_back(type);
        }

        offset = value_offset + length;
        offset += (4U - (offset % 4U)) % 4U;
    }

    if (offset != packet.size())
    {
        spdlog::debug("stun reject trailing bytes offset {} size {}", offset, packet.size());
        return std::nullopt;
    }
    return result;
}

bool validate_stun_binding_authentication(std::span<const std::uint8_t> packet,
                                          const stun_binding_attributes& attributes,
                                          std::string_view expected_username,
                                          std::string_view password)
{
    if (!attributes.username.has_value())
    {
        spdlog::debug("stun reject missing username");
        return false;
    }
    if (*attributes.username != expected_username)
    {
        spdlog::debug("stun reject username actual {} expected {}", *attributes.username, expected_username);
        return false;
    }
    if (!attributes.message_integrity_offset.has_value())
    {
        spdlog::debug("stun reject missing message integrity");
        return false;
    }
    if (!verify_message_integrity(packet, *attributes.message_integrity_offset, password))
    {
        spdlog::debug("stun reject message integrity failed");
        return false;
    }
    if (!attributes.fingerprint_offset.has_value())
    {
        spdlog::debug("stun reject missing fingerprint");
        return false;
    }
    if (*attributes.fingerprint_offset + 8U != packet.size() || !verify_fingerprint(packet, *attributes.fingerprint_offset))
    {
        spdlog::debug("stun reject fingerprint failed");
        return false;
    }
    return true;
}

}    // namespace

bool is_stun_message(std::span<const std::uint8_t> packet)
{
    return packet.size() >= stun_header_size && (packet[0] & 0xc0U) == 0 && read_u32(packet.data() + 4U) == stun_magic_cookie;
}

std::optional<stun_binding_request> parse_stun_binding_request(std::span<const std::uint8_t> packet,
                                                               std::string_view expected_username,
                                                               std::string_view password)
{
    if (!is_stun_message(packet))
    {
        spdlog::debug("stun reject invalid header size {}", packet.size());
        return std::nullopt;
    }

    const auto message_type = read_u16(packet.data());
    if (message_type != stun_binding_request_type)
    {
        spdlog::debug("stun reject message type 0x{:04x}", message_type);
        return std::nullopt;
    }

    const auto body_size = static_cast<std::size_t>(read_u16(packet.data() + 2U));
    if (body_size != packet.size() - stun_header_size)
    {
        spdlog::debug("stun reject length header {} actual {}", body_size, packet.size() - stun_header_size);
        return std::nullopt;
    }

    auto attributes = scan_stun_attributes(packet);
    if (!attributes || !validate_stun_binding_authentication(packet, *attributes, expected_username, password))
    {
        return std::nullopt;
    }

    stun_binding_request request{
        .transaction_id = {},
        .unknown_required_attributes = std::move(attributes->unknown_required_attributes),
        .priority = attributes->priority,
        .use_candidate = attributes->use_candidate,
        .ice_controlling = attributes->ice_controlling,
        .ice_controlled = attributes->ice_controlled,
    };
    std::copy_n(packet.data() + 8U, request.transaction_id.size(), request.transaction_id.begin());
    spdlog::trace("stun binding request authenticated use_candidate {}", attributes->use_candidate);
    return request;
}

std::vector<std::uint8_t> make_stun_binding_success_response(const stun_binding_request& request,
                                                             const boost::asio::ip::udp::endpoint& remote_endpoint,
                                                             std::string_view password)
{
    auto packet = make_header(stun_binding_success_response_type, request.transaction_id);
    const auto mapped_address = xor_mapped_address(remote_endpoint, request.transaction_id);
    append_attribute(packet, stun_attribute_xor_mapped_address, mapped_address);

    if (!append_integrity_and_fingerprint(packet, password))
    {
        return {};
    }
    return packet;
}

std::vector<std::uint8_t> make_stun_binding_error_response(const stun_binding_request& request,
                                                           std::uint16_t error_code,
                                                           std::string_view reason,
                                                           std::span<const std::uint16_t> unknown_attributes,
                                                           std::string_view password)
{
    auto packet = make_header(stun_binding_error_response_type, request.transaction_id);

    std::vector<std::uint8_t> error_value{0, 0, static_cast<std::uint8_t>(error_code / 100U), static_cast<std::uint8_t>(error_code % 100U)};
    error_value.insert(error_value.end(), reason.begin(), reason.end());
    append_attribute(packet, stun_attribute_error_code, error_value);

    if (!unknown_attributes.empty())
    {
        std::vector<std::uint8_t> unknown_value;
        unknown_value.reserve(unknown_attributes.size() * 2U);
        for (const auto type : unknown_attributes)
        {
            append_u16(unknown_value, type);
        }
        append_attribute(packet, stun_attribute_unknown_attributes, unknown_value);
    }

    if (!append_integrity_and_fingerprint(packet, password))
    {
        return {};
    }
    return packet;
}

}    // namespace media_server
