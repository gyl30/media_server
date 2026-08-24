#include <array>
#include <cctype>
#include <optional>
#include <algorithm>

#include "media/rtsp/rtsp_sdp.h"

extern "C"
{
#include "base64.h"
}

namespace media_server
{

bool rtsp_sdp_iequals(const char* value, std::string_view expected)
{
    if (value == nullptr)
    {
        return false;
    }

    const std::string_view actual(value);
    return actual.size() == expected.size() &&
           std::equal(actual.begin(),
                      actual.end(),
                      expected.begin(),
                      [](unsigned char left, unsigned char right) { return std::tolower(left) == std::tolower(right); });
}

std::optional<std::uint16_t> rtsp_sdp_opus_channel_count(const char* fmtp)
{
    if (fmtp == nullptr)
    {
        return 1;
    }

    std::string_view parameters(fmtp);
    if (const auto space = parameters.find(' '); space != std::string_view::npos)
    {
        parameters.remove_prefix(space + 1U);
    }

    while (!parameters.empty())
    {
        const auto separator = parameters.find(';');
        auto parameter = parameters.substr(0, separator);
        const auto first = parameter.find_first_not_of(" \t");
        if (first != std::string_view::npos)
        {
            parameter.remove_prefix(first);
            const auto last = parameter.find_last_not_of(" \t");
            parameter = parameter.substr(0, last + 1U);
        }

        const auto equals = parameter.find('=');
        if (equals != std::string_view::npos)
        {
            auto name = parameter.substr(0, equals);
            auto value = parameter.substr(equals + 1U);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0)
            {
                name.remove_suffix(1U);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
            {
                value.remove_prefix(1U);
            }
            const bool sprop_stereo = name.size() == std::string_view("sprop-stereo").size() &&
                                      std::equal(name.begin(),
                                                 name.end(),
                                                 std::string_view("sprop-stereo").begin(),
                                                 [](unsigned char left, unsigned char right) { return std::tolower(left) == std::tolower(right); });
            if (sprop_stereo)
            {
                if (value == "0")
                {
                    return 1;
                }
                if (value == "1")
                {
                    return 2;
                }
                return std::nullopt;
            }
        }

        if (separator == std::string_view::npos)
        {
            break;
        }
        parameters.remove_prefix(separator + 1U);
    }
    return 1;
}

bool rtsp_sdp_append_parameter_sets(std::vector<std::uint8_t>& config, std::string_view encoded)
{
    constexpr std::array<std::uint8_t, 4> start_code{0x00, 0x00, 0x00, 0x01};
    while (!encoded.empty())
    {
        const auto comma = encoded.find(',');
        const auto value = encoded.substr(0, comma);
        if (value.empty())
        {
            return false;
        }

        std::vector<std::uint8_t> decoded((value.size() + 3U) / 4U * 3U);
        const auto bytes = base64_decode(decoded.data(), value.data(), value.size());
        if (bytes == 0)
        {
            return false;
        }
        decoded.resize(bytes);
        config.insert(config.end(), start_code.begin(), start_code.end());
        config.insert(config.end(), decoded.begin(), decoded.end());

        if (comma == std::string_view::npos)
        {
            break;
        }
        encoded.remove_prefix(comma + 1U);
    }
    return !config.empty();
}

}    // namespace media_server
