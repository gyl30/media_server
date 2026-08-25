#include <array>
#include <cctype>
#include <optional>
#include <algorithm>
#include <utility>

#include "media/codec/codec_utils.h"
#include "media/rtsp/rtsp_sdp.h"

extern "C"
{
#include "base64.h"
#include "rtp-profile.h"
#include "sdp-a-fmtp.h"
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

std::optional<media_track> rtsp_sdp_track_from_format(
    const char* media, int payload_type, int rate, const char* encoding, const char* fmtp, track_id id)
{
    const bool video = rtsp_sdp_iequals(media, "video");
    const bool audio = rtsp_sdp_iequals(media, "audio");
    if (video && rtsp_sdp_iequals(encoding, "H264"))
    {
        sdp_a_fmtp_h264_t parameters{};
        auto payload = payload_type;
        std::vector<std::uint8_t> config;
        if (fmtp != nullptr && sdp_a_fmtp_h264(fmtp, &payload, &parameters) == 0 && rtsp_sdp_append_parameter_sets(config, parameters.sprop_parameter_sets) &&
            !h264_annex_b_to_avcc(config).empty())
        {
            return media_track{.id = id,
                               .kind = media_kind::video,
                               .codec = codec_id::h264,
                               .clock_rate = 90'000,
                               .codec_config = std::move(config)};
        }
    }
    else if (video && (rtsp_sdp_iequals(encoding, "H265") || rtsp_sdp_iequals(encoding, "HEVC")))
    {
        sdp_a_fmtp_h265_t parameters{};
        auto payload = payload_type;
        std::vector<std::uint8_t> config;
        if (fmtp != nullptr && sdp_a_fmtp_h265(fmtp, &payload, &parameters) == 0 && rtsp_sdp_append_parameter_sets(config, parameters.sprop_vps) &&
            rtsp_sdp_append_parameter_sets(config, parameters.sprop_sps) && rtsp_sdp_append_parameter_sets(config, parameters.sprop_pps) &&
            !h265_annex_b_to_hvcc(config).empty())
        {
            return media_track{.id = id,
                               .kind = media_kind::video,
                               .codec = codec_id::h265,
                               .clock_rate = 90'000,
                               .codec_config = std::move(config)};
        }
    }
    else if (audio && rtsp_sdp_iequals(encoding, "MPEG4-GENERIC"))
    {
        sdp_a_fmtp_mpeg4_t parameters{};
        auto payload = payload_type;
        if (fmtp != nullptr && sdp_a_fmtp_mpeg4(fmtp, &payload, &parameters) == 0)
        {
            const std::string_view encoded(parameters.config);
            if (!encoded.empty() && encoded.size() % 2U == 0U &&
                std::all_of(encoded.begin(), encoded.end(), [](char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; }))
            {
                std::vector<std::uint8_t> config(encoded.size() / 2U);
                static_cast<void>(base16_decode(config.data(), encoded.data(), encoded.size()));
                if (const auto aac = parse_aac_asc(config))
                {
                    return media_track{.id = id,
                                       .kind = media_kind::audio,
                                       .codec = codec_id::aac,
                                       .clock_rate = aac->sample_rate,
                                       .channel_count = aac->channel_count,
                                       .codec_config = std::move(config)};
                }
            }
        }
    }
    else if (audio && rtsp_sdp_iequals(encoding, "opus") && rate == 48'000)
    {
        if (const auto channels = rtsp_sdp_opus_channel_count(fmtp))
        {
            return media_track{.id = id,
                               .kind = media_kind::audio,
                               .codec = codec_id::opus,
                               .clock_rate = 48'000,
                               .channel_count = *channels,
                               .codec_config = {}};
        }
    }
    else if (audio && rate == 8'000 &&
             ((payload_type == RTP_PAYLOAD_PCMA && (encoding == nullptr || encoding[0] == '\0' || rtsp_sdp_iequals(encoding, "PCMA"))) ||
              (payload_type == RTP_PAYLOAD_PCMU && (encoding == nullptr || encoding[0] == '\0' || rtsp_sdp_iequals(encoding, "PCMU")))))
    {
        return media_track{.id = id,
                           .kind = media_kind::audio,
                           .codec = payload_type == RTP_PAYLOAD_PCMA ? codec_id::g711a : codec_id::g711u,
                           .clock_rate = 8'000,
                           .channel_count = 1,
                           .codec_config = {}};
    }
    return std::nullopt;
}

}    // namespace media_server
