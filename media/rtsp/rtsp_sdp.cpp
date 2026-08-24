#include <array>
#include <cctype>
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
