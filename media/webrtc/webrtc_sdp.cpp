#include "media/webrtc/webrtc_sdp.h"

extern "C"
{
#include "mpeg4-hevc.h"
#include "rtp-profile.h"
#include "sdp-a-rtpmap.h"
#include "sdp-a-webrtc.h"
#include "sdp.h"
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace media_server
{
namespace
{

constexpr std::string_view mid_extension_uri = "urn:ietf:params:rtp-hdrext:sdes:mid";
constexpr std::size_t max_mid_size = 16;

struct sdp_deleter
{
    void operator()(sdp_t* value) const noexcept { sdp_destroy(value); }
};

using sdp_ptr = std::unique_ptr<sdp_t, sdp_deleter>;

std::string lower_copy(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

enum class h264_profile
{
    constrained_baseline,
    baseline,
    main,
    constrained_high,
    high,
    predictive_high_444,
};

struct h264_profile_level
{
    h264_profile profile;
    int level_rank{};
};

struct h265_profile_tier_level
{
    int profile_space{};
    int profile{};
    int tier{};
    int level{};
};

std::vector<std::string_view> split_words(std::string_view value)
{
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin < value.size())
    {
        while (begin < value.size() && value[begin] == ' ')
        {
            ++begin;
        }
        if (begin == value.size())
        {
            break;
        }
        const auto end = value.find(' ', begin);
        if (end == std::string_view::npos)
        {
            result.push_back(value.substr(begin));
            break;
        }
        result.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

std::string attribute(sdp_t* sdp, int media, const char* name, const char* session_value = nullptr)
{
    const char* value = sdp_media_attribute_find(sdp, media, name);
    if (value == nullptr)
    {
        value = session_value;
    }
    return value == nullptr ? std::string{} : std::string(value);
}

std::string direction(int mode)
{
    switch (mode)
    {
        case SDP_A_SENDONLY:
            return "sendonly";
        case SDP_A_RECVONLY:
            return "recvonly";
        case SDP_A_INACTIVE:
            return "inactive";
        case SDP_A_SENDRECV:
        default:
            return "sendrecv";
    }
}

void on_rtpmap(void* param, const char*, const char* value)
{
    if (param == nullptr || value == nullptr)
    {
        return;
    }

    int payload_type = 0;
    int clock_rate = 0;
    char encoding[16]{};
    char parameters[64]{};
    if (sdp_a_rtpmap(value, &payload_type, encoding, &clock_rate, parameters) != 0 || clock_rate <= 0)
    {
        return;
    }

    std::uint16_t channel_count = 0;
    if (parameters[0] != '\0')
    {
        unsigned int channels = 0;
        const std::string_view text(parameters);
        const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), channels);
        if (error == std::errc{} && pointer == text.data() + text.size() && channels <= 65'535U)
        {
            channel_count = static_cast<std::uint16_t>(channels);
        }
    }

    auto* media = static_cast<webrtc_media_offer*>(param);
    if (payload_type < 0 || payload_type > 127 ||
        std::find(media->payload_types.begin(), media->payload_types.end(), payload_type) == media->payload_types.end())
    {
        return;
    }
    media->codecs.push_back(webrtc_codec_offer{
        .payload_type = payload_type,
        .encoding_name = encoding,
        .clock_rate = static_cast<std::uint32_t>(clock_rate),
        .channel_count = channel_count,
        .format_parameters = {},
    });
}

void on_extmap(void* param, const char*, const char* value)
{
    if (param == nullptr || value == nullptr)
    {
        return;
    }

    int extension_id = 0;
    int extension_direction = SDP_A_SENDRECV;
    char uri[128]{};
    if (sdp_a_extmap(value, static_cast<int>(std::char_traits<char>::length(value)), &extension_id, &extension_direction, uri) != 0 ||
        extension_id <= 0 || extension_id > 255 ||
        (extension_direction != SDP_A_SENDRECV && extension_direction != SDP_A_RECVONLY) || uri != mid_extension_uri)
    {
        return;
    }

    static_cast<webrtc_media_offer*>(param)->mid_extension_id = extension_id;
}

void on_fmtp(void* param, const char*, const char* value)
{
    if (param == nullptr || value == nullptr)
    {
        return;
    }

    const std::string_view text(value);
    const auto space = text.find(' ');
    if (space == std::string_view::npos)
    {
        return;
    }

    int payload_type = 0;
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + space, payload_type);
    if (error != std::errc{} || pointer != text.data() + space)
    {
        return;
    }

    auto* media = static_cast<webrtc_media_offer*>(param);
    const auto iterator = std::find_if(
        media->codecs.begin(), media->codecs.end(), [payload_type](const webrtc_codec_offer& codec) { return codec.payload_type == payload_type; });
    if (iterator != media->codecs.end())
    {
        iterator->format_parameters = std::string(text.substr(space + 1));
    }
}

std::optional<std::string_view> parameter_value(std::string_view parameters, std::string_view name)
{
    std::size_t begin = 0;
    while (begin < parameters.size())
    {
        auto end = parameters.find(';', begin);
        if (end == std::string_view::npos)
        {
            end = parameters.size();
        }
        auto item = parameters.substr(begin, end - begin);
        while (!item.empty() && item.front() == ' ')
        {
            item.remove_prefix(1);
        }
        const auto equal = item.find('=');
        if (equal != std::string_view::npos && lower_copy(item.substr(0, equal)) == lower_copy(name))
        {
            return item.substr(equal + 1);
        }
        begin = end + 1;
    }
    return std::nullopt;
}

bool has_parameter(std::string_view parameters, std::string_view name, std::string_view value)
{
    return parameter_value(parameters, name) == value;
}

std::optional<int> decimal_parameter(std::string_view parameters, std::string_view name, int default_value, int maximum)
{
    const auto text = parameter_value(parameters, name);
    if (!text)
    {
        return default_value;
    }

    int value = 0;
    const auto [pointer, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || pointer != text->data() + text->size() || value < 0 || value > maximum)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<h264_profile_level> parse_h264_profile_level(std::string_view text)
{
    if (text.size() != 6U)
    {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0)
    {
        return std::nullopt;
    }

    const auto profile_idc = static_cast<std::uint8_t>(value >> 16U);
    const auto profile_iop = static_cast<std::uint8_t>(value >> 8U);
    const auto level_idc = static_cast<std::uint8_t>(value);

    // RFC 6184 允许不同 profile_idc/profile-iop 组合表示同一子配置，不能直接比较六位字符串。
    struct profile_pattern
    {
        std::uint8_t id;
        std::uint8_t mask;
        std::uint8_t value;
        h264_profile profile;
    };
    constexpr profile_pattern patterns[]{
        {0x42U, 0x4fU, 0x40U, h264_profile::constrained_baseline},
        {0x4dU, 0x8fU, 0x80U, h264_profile::constrained_baseline},
        {0x58U, 0xcfU, 0xc0U, h264_profile::constrained_baseline},
        {0x42U, 0x4fU, 0x00U, h264_profile::baseline},
        {0x58U, 0xcfU, 0x80U, h264_profile::baseline},
        {0x4dU, 0xafU, 0x00U, h264_profile::main},
        {0x64U, 0xffU, 0x0cU, h264_profile::constrained_high},
        {0x64U, 0xffU, 0x00U, h264_profile::high},
        {0xf4U, 0xffU, 0x00U, h264_profile::predictive_high_444},
    };
    const auto pattern = std::find_if(std::begin(patterns),
                                      std::end(patterns),
                                      [profile_idc, profile_iop](const profile_pattern& candidate)
                                      { return candidate.id == profile_idc && (profile_iop & candidate.mask) == candidate.value; });
    if (pattern == std::end(patterns))
    {
        return std::nullopt;
    }

    const bool level_1b = level_idc == 11U && (profile_idc == 0x42U || profile_idc == 0x4dU || profile_idc == 0x58U) &&
        (profile_iop & 0x10U) != 0U;
    const bool valid_level = level_1b || level_idc == 10U || level_idc == 11U || level_idc == 12U || level_idc == 13U ||
        level_idc == 20U || level_idc == 21U || level_idc == 22U || level_idc == 30U || level_idc == 31U || level_idc == 32U ||
        level_idc == 40U || level_idc == 41U || level_idc == 42U || level_idc == 50U || level_idc == 51U || level_idc == 52U;
    if (!valid_level)
    {
        return std::nullopt;
    }

    const int level_rank = level_1b ? 11 : (level_idc >= 11U ? static_cast<int>(level_idc) + 1 : static_cast<int>(level_idc));
    return h264_profile_level{.profile = pattern->profile, .level_rank = level_rank};
}

std::optional<h265_profile_tier_level> source_h265_profile_tier_level(const media_track& track)
{
    mpeg4_hevc_t configuration{};
    if (track.codec_config.empty() || mpeg4_hevc_from_nalu(track.codec_config.data(), track.codec_config.size(), &configuration) < 0 ||
        configuration.numOfArrays < 3)
    {
        return std::nullopt;
    }

    return h265_profile_tier_level{
        .profile_space = configuration.general_profile_space,
        .profile = configuration.general_profile_idc,
        .tier = configuration.general_tier_flag,
        .level = configuration.general_level_idc,
    };
}

bool rtcp_mux_payload_type_allowed(int payload_type)
{
    return payload_type >= 0 && payload_type <= 127 && (payload_type < 64 || payload_type > 95);
}

const webrtc_codec_offer* find_h264(const webrtc_media_offer& media, const h264_profile_level& source)
{
    const auto iterator = std::find_if(
        media.codecs.begin(),
        media.codecs.end(),
        [&source](const webrtc_codec_offer& codec)
        {
            if (!rtcp_mux_payload_type_allowed(codec.payload_type) || lower_copy(codec.encoding_name) != "h264" || codec.clock_rate != 90'000U ||
                !has_parameter(codec.format_parameters, "packetization-mode", "1"))
            {
                return false;
            }
            const auto profile_level_id = parameter_value(codec.format_parameters, "profile-level-id");
            const auto offered = parse_h264_profile_level(profile_level_id.value_or("42000a"));
            return offered && offered->profile == source.profile && source.level_rank <= offered->level_rank;
        });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_h265(const webrtc_media_offer& media, const h265_profile_tier_level& source)
{
    const auto iterator = std::find_if(
        media.codecs.begin(),
        media.codecs.end(),
        [&source](const webrtc_codec_offer& codec)
        {
            const auto encoding = lower_copy(codec.encoding_name);
            if (!rtcp_mux_payload_type_allowed(codec.payload_type) || (encoding != "h265" && encoding != "hevc") || codec.clock_rate != 90'000U)
            {
                return false;
            }

            if (parameter_value(codec.format_parameters, "profile-compatibility-indicator") ||
                parameter_value(codec.format_parameters, "interop-constraints"))
            {
                return false;
            }

            // RFC 7798 对省略的 profile-space/profile-id/tier-flag/level-id 分别推导 0/1/0/93，tx-mode 推导 SRST。
            const auto profile_space = decimal_parameter(codec.format_parameters, "profile-space", 0, 3);
            const auto profile = decimal_parameter(codec.format_parameters, "profile-id", 1, 31);
            const auto tier = decimal_parameter(codec.format_parameters, "tier-flag", 0, 1);
            const auto level = decimal_parameter(codec.format_parameters, "level-id", 93, 255);
            const auto tx_mode = parameter_value(codec.format_parameters, "tx-mode");
            return profile_space && profile && tier && level && *profile_space == source.profile_space && *profile == source.profile &&
                *tier == source.tier && source.level <= *level && (!tx_mode || lower_copy(*tx_mode) == "srst");
        });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_av1(const webrtc_media_offer& media)
{
    const auto iterator = std::find_if(
        media.codecs.begin(),
        media.codecs.end(),
        [](const webrtc_codec_offer& codec)
        {
            if (!rtcp_mux_payload_type_allowed(codec.payload_type) || lower_copy(codec.encoding_name) != "av1" || codec.clock_rate != 90'000U)
            {
                return false;
            }
            const auto profile = decimal_parameter(codec.format_parameters, "profile", 0, 2);
            const auto level_idx = decimal_parameter(codec.format_parameters, "level-idx", 5, 31);
            const auto tier = decimal_parameter(codec.format_parameters, "tier", 0, 1);
            // 浏览器的 level-idx 目前不能作为实际解码上限；WHEP 固定发送 Main profile/Main tier，这里只校验 AV1 fmtp。
            return profile && level_idx && tier;
        });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_opus(const webrtc_media_offer& media)
{
    const auto iterator = std::find_if(media.codecs.begin(),
                                       media.codecs.end(),
                                       [](const webrtc_codec_offer& codec)
                                       {
                                           return rtcp_mux_payload_type_allowed(codec.payload_type) && lower_copy(codec.encoding_name) == "opus" &&
                                               codec.clock_rate == 48'000U && codec.channel_count == 2;
                                       });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_g711(const webrtc_media_offer& media, codec_id codec)
{
    const auto payload_type = codec == codec_id::g711a ? RTP_PAYLOAD_PCMA : RTP_PAYLOAD_PCMU;
    const auto encoding = codec == codec_id::g711a ? "pcma" : "pcmu";
    const auto iterator = std::find_if(media.codecs.begin(),
                                       media.codecs.end(),
                                       [payload_type, encoding](const webrtc_codec_offer& offered)
                                       {
                                           return offered.payload_type == payload_type && lower_copy(offered.encoding_name) == encoding &&
                                               offered.clock_rate == 8'000U && (offered.channel_count == 0 || offered.channel_count == 1);
                                       });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

int opus_output_channel_count(const webrtc_codec_offer& codec, const media_track& track)
{
    if (track.channel_count >= 2 && has_parameter(codec.format_parameters, "stereo", "1"))
    {
        return 2;
    }
    return 1;
}

bool opus_passthrough_compatible(const webrtc_codec_offer& codec, const media_track& track)
{
    if (track.channel_count == 2 && !has_parameter(codec.format_parameters, "stereo", "1"))
    {
        return false;
    }

    if (parameter_value(codec.format_parameters, "maxplaybackrate"))
    {
        const auto rate = decimal_parameter(codec.format_parameters, "maxplaybackrate", 48'000, 48'000);
        if (!rate || *rate != 48'000)
        {
            return false;
        }
    }

    const auto bitrate = decimal_parameter(codec.format_parameters, "maxaveragebitrate", 0, 510'000);
    if (!bitrate || *bitrate != 510'000)
    {
        return false;
    }
    return true;
}

std::string h264_profile_level_id(const media_track& track)
{
    const auto& data = track.codec_config;
    for (std::size_t index = 0; index + 8U <= data.size(); ++index)
    {
        std::size_t nalu = 0;
        if (index + 4U <= data.size() && data[index] == 0 && data[index + 1U] == 0 && data[index + 2U] == 0 && data[index + 3U] == 1)
        {
            nalu = index + 4U;
        }
        else if (index + 3U <= data.size() && data[index] == 0 && data[index + 1U] == 0 && data[index + 2U] == 1)
        {
            nalu = index + 3U;
        }
        else
        {
            continue;
        }

        if (nalu + 4U > data.size() || (data[nalu] & 0x1FU) != 7U)
        {
            continue;
        }

        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::nouppercase << std::setw(2) << static_cast<unsigned int>(data[nalu + 1U]) << std::setw(2)
               << static_cast<unsigned int>(data[nalu + 2U]) << std::setw(2) << static_cast<unsigned int>(data[nalu + 3U]);
        return stream.str();
    }
    return {};
}

bool bundle_contains(const webrtc_offer& offer, std::string_view mid)
{
    return std::find(offer.bundle_mids.begin(), offer.bundle_mids.end(), mid) != offer.bundle_mids.end();
}

bool bundle_media_supported(const webrtc_media_offer& media)
{
    const bool transport_candidate = media.port != 0 && !media.bundle_only;
    const bool bundle_only = media.port == 0 && media.bundle_only;
    if ((!transport_candidate && !bundle_only) || lower_copy(media.protocol) != "udp/tls/rtp/savpf" || !media.mid_extension_id.has_value() ||
        media.mid.empty())
    {
        return false;
    }
    return bundle_only || (media.rtcp_mux && lower_copy(media.setup) == "actpass");
}

void append_transport(std::ostringstream& answer, const webrtc_answer_config& config)
{
    answer << "a=ice-ufrag:" << config.ice_ufrag << "\r\n";
    answer << "a=ice-pwd:" << config.ice_pwd << "\r\n";
    answer << "a=fingerprint:sha-256 " << config.fingerprint << "\r\n";
    answer << "a=setup:passive\r\n";
    answer << "a=candidate:1 1 UDP 2130706431 " << config.address.to_string() << ' ' << config.port << " typ host\r\n";
    answer << "a=end-of-candidates\r\n";
}

}    // namespace

std::optional<webrtc_offer> parse_webrtc_offer(std::string_view text)
{
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }

    sdp_ptr sdp(sdp_parse(text.data(), static_cast<int>(text.size())));
    if (!sdp)
    {
        return std::nullopt;
    }

    webrtc_offer result;
    bool bundle_found = false;
    const auto attribute_count = sdp_attribute_count(sdp.get());
    for (int index = 0; index < attribute_count; ++index)
    {
        const char* name = nullptr;
        const char* value = nullptr;
        if (sdp_attribute_get(sdp.get(), index, &name, &value) != 0 || name == nullptr || value == nullptr || std::string_view(name) != "group")
        {
            continue;
        }

        const auto fields = split_words(value);
        if (fields.empty() || lower_copy(fields.front()) != "bundle")
        {
            continue;
        }
        if (bundle_found)
        {
            return std::nullopt;
        }
        bundle_found = true;
        for (std::size_t field = 1; field < fields.size(); ++field)
        {
            result.bundle_mids.emplace_back(fields[field]);
        }
    }

    const char* session_ice_ufrag = sdp_attribute_find(sdp.get(), "ice-ufrag");
    const char* session_ice_pwd = sdp_attribute_find(sdp.get(), "ice-pwd");
    const char* session_fingerprint = sdp_attribute_find(sdp.get(), "fingerprint");
    const char* session_setup = sdp_attribute_find(sdp.get(), "setup");

    const auto media_count = sdp_media_count(sdp.get());
    if (media_count <= 0)
    {
        return std::nullopt;
    }

    for (int index = 0; index < media_count; ++index)
    {
        const char* type = sdp_media_type(sdp.get(), index);
        const char* protocol = sdp_media_proto(sdp.get(), index);
        const char* mid = sdp_media_attribute_find(sdp.get(), index, "mid");
        if (type == nullptr || protocol == nullptr || mid == nullptr || *mid == '\0' || std::char_traits<char>::length(mid) > max_mid_size)
        {
            return std::nullopt;
        }
        int port = 0;
        if (sdp_media_port(sdp.get(), index, &port, 1) != 1)
        {
            return std::nullopt;
        }

        webrtc_media_offer media{
            .type = type,
            .port = port,
            .protocol = protocol,
            .mid = mid,
            .direction = direction(sdp_media_mode(sdp.get(), index)),
            .setup = attribute(sdp.get(), index, "setup", session_setup),
            .ice_ufrag = attribute(sdp.get(), index, "ice-ufrag", session_ice_ufrag),
            .ice_pwd = attribute(sdp.get(), index, "ice-pwd", session_ice_pwd),
            .fingerprint = attribute(sdp.get(), index, "fingerprint", session_fingerprint),
            .rtcp_mux = sdp_media_attribute_find(sdp.get(), index, "rtcp-mux") != nullptr,
            .bundle_only = sdp_media_attribute_find(sdp.get(), index, "bundle-only") != nullptr,
            .mid_extension_id = std::nullopt,
            .max_packet_time_ms = std::nullopt,
            .formats = {},
            .payload_types = {},
            .codecs = {},
        };

        if (const char* maxptime = sdp_media_attribute_find(sdp.get(), index, "maxptime"); maxptime != nullptr)
        {
            int value = 0;
            const std::string_view maxptime_text(maxptime);
            const auto [pointer, error] = std::from_chars(maxptime_text.data(), maxptime_text.data() + maxptime_text.size(), value);
            if (error == std::errc{} && pointer == maxptime_text.data() + maxptime_text.size() && value > 0)
            {
                media.max_packet_time_ms = value;
            }
        }

        const auto format_count = sdp_media_formats(sdp.get(), index, nullptr, 0);
        if (format_count <= 0)
        {
            return std::nullopt;
        }
        media.formats.reserve(static_cast<std::size_t>(format_count));
        for (int format = 0; format < format_count; ++format)
        {
            const char* value = sdp_media_format(sdp.get(), index, format);
            if (value == nullptr || *value == '\0')
            {
                return std::nullopt;
            }
            media.formats.emplace_back(value);
        }
        media.payload_types.resize(static_cast<std::size_t>(format_count));
        static_cast<void>(sdp_media_formats(sdp.get(), index, media.payload_types.data(), format_count));

        static_cast<void>(sdp_media_attribute_list(sdp.get(), index, "rtpmap", &on_rtpmap, &media));
        static_cast<void>(sdp_media_attribute_list(sdp.get(), index, "fmtp", &on_fmtp, &media));
        static_cast<void>(sdp_media_attribute_list(sdp.get(), index, "extmap", &on_extmap, &media));
        for (const auto payload_type : media.payload_types)
        {
            if ((payload_type == RTP_PAYLOAD_PCMU || payload_type == RTP_PAYLOAD_PCMA) &&
                std::none_of(media.codecs.begin(), media.codecs.end(), [payload_type](const webrtc_codec_offer& codec) {
                    return codec.payload_type == payload_type;
                }))
            {
                media.codecs.push_back(webrtc_codec_offer{
                    .payload_type = payload_type,
                    .encoding_name = payload_type == RTP_PAYLOAD_PCMU ? "PCMU" : "PCMA",
                    .clock_rate = 8'000,
                    .channel_count = 1,
                    .format_parameters = {},
                });
            }
        }
        if (std::any_of(result.media.begin(), result.media.end(), [&media](const webrtc_media_offer& existing) { return existing.mid == media.mid; }))
        {
            return std::nullopt;
        }
        result.media.push_back(std::move(media));
    }

    for (const auto& mid : result.bundle_mids)
    {
        if (std::count(result.bundle_mids.begin(), result.bundle_mids.end(), mid) != 1 ||
            std::none_of(result.media.begin(), result.media.end(), [&mid](const webrtc_media_offer& media) { return media.mid == mid; }))
        {
            return std::nullopt;
        }
    }

    return result;
}

std::optional<webrtc_answer> make_webrtc_answer(const webrtc_offer& offer, const std::vector<media_track>& tracks, const webrtc_answer_config& config)
{
    if (config.port == 0 || config.stream_id.empty() || config.ice_ufrag.empty() || config.ice_pwd.empty() || config.fingerprint.empty())
    {
        return std::nullopt;
    }

    const auto video_iterator = std::find_if(
        tracks.begin(),
        tracks.end(),
        [](const media_track& track) { return track.kind == media_kind::video && (track.codec == codec_id::h264 || track.codec == codec_id::h265); });
    const auto* video_track = video_iterator == tracks.end() ? nullptr : &*video_iterator;
    const auto audio_iterator = std::find_if(
        tracks.begin(),
        tracks.end(),
        [](const media_track& track)
        {
            return track.kind == media_kind::audio &&
                (track.codec == codec_id::aac ||
                 (track.codec == codec_id::opus && track.clock_rate == 48'000 && (track.channel_count == 1 || track.channel_count == 2) &&
                  track.codec_config.empty()) ||
                 ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) && track.clock_rate == 8'000 && track.channel_count == 1 &&
                  track.codec_config.empty()));
        });
    const auto* audio_track = audio_iterator == tracks.end() ? nullptr : &*audio_iterator;
    const auto address_type = config.address.is_v6() ? "IP6" : "IP4";

    if (offer.bundle_mids.empty())
    {
        return std::nullopt;
    }
    const auto transport_mid = offer.bundle_mids.front();
    const auto transport_media = std::find_if(
        offer.media.begin(), offer.media.end(), [&transport_mid](const webrtc_media_offer& media) { return media.mid == transport_mid; });
    if (transport_media == offer.media.end() || transport_media->port == 0)
    {
        return std::nullopt;
    }
    const auto transport_index = static_cast<std::size_t>(std::distance(offer.media.begin(), transport_media));
    std::vector<std::size_t> negotiation_order;
    negotiation_order.reserve(offer.media.size());
    negotiation_order.push_back(transport_index);
    for (std::size_t index = 0; index < offer.media.size(); ++index)
    {
        if (index != transport_index)
        {
            negotiation_order.push_back(index);
        }
    }

    std::vector<std::string> media_answers(offer.media.size());
    std::vector<std::string> accepted_mids;
    std::optional<codec_id> video_codec;
    std::optional<codec_id> audio_codec;
    std::optional<int> video_payload_type;
    std::optional<int> audio_payload_type;
    std::optional<std::string> video_mid;
    std::optional<std::string> audio_mid;
    std::optional<int> video_mid_extension_id;
    std::optional<int> audio_mid_extension_id;
    std::optional<int> bundle_mid_extension_id;
    std::optional<int> audio_channel_count;
    std::optional<int> audio_bitrate;
    std::optional<int> audio_max_playback_rate;
    for (const auto media_index : negotiation_order)
    {
        const auto& media = offer.media[media_index];
        std::ostringstream media_answer;
        const auto media_direction = lower_copy(media.direction);
        const bool can_receive = bundle_contains(offer, media.mid) && bundle_media_supported(media) &&
            (!bundle_mid_extension_id || *bundle_mid_extension_id == *media.mid_extension_id) &&
            (media_direction == "sendrecv" || media_direction == "recvonly");
        const webrtc_codec_offer* codec = nullptr;
        std::string profile_level_id;

        if (can_receive && lower_copy(media.type) == "video" && video_track != nullptr && !video_payload_type.has_value())
        {
            if (config.video.codec == output_video_codec::av1)
            {
                codec = find_av1(media);
            }
            else if (video_track->codec == codec_id::h264)
            {
                profile_level_id = h264_profile_level_id(*video_track);
                const auto source = parse_h264_profile_level(profile_level_id);
                if (source)
                {
                    codec = find_h264(media, *source);
                }
            }
            else if (video_track->codec == codec_id::h265)
            {
                const auto source = source_h265_profile_tier_level(*video_track);
                if (source)
                {
                    codec = find_h265(media, *source);
                }
            }
        }
        else if (can_receive && lower_copy(media.type) == "audio" && audio_track != nullptr && !audio_payload_type.has_value())
        {
            codec = audio_track->codec == codec_id::g711a || audio_track->codec == codec_id::g711u ? find_g711(media, audio_track->codec)
                                                                                                  : find_opus(media);
            if (codec != nullptr && audio_track->codec == codec_id::opus &&
                (!opus_passthrough_compatible(*codec, *audio_track) || (media.max_packet_time_ms && *media.max_packet_time_ms != 120)))
            {
                codec = nullptr;
            }
        }

        if (codec != nullptr && ((video_payload_type && *video_payload_type == codec->payload_type) ||
                                 (audio_payload_type && *audio_payload_type == codec->payload_type)))
        {
            codec = nullptr;
        }

        if (codec == nullptr)
        {
            if (media.mid == transport_mid)
            {
                return std::nullopt;
            }
            spdlog::debug("webrtc answer reject media type {} mid {}", media.type, media.mid);
            media_answer << "m=" << media.type << " 0 " << media.protocol;
            for (const auto& format : media.formats)
            {
                media_answer << ' ' << format;
            }
            media_answer << "\r\n";
            media_answer << "c=IN " << address_type << ' ' << (config.address.is_v6() ? "::" : "0.0.0.0") << "\r\n";
            media_answer << "a=mid:" << media.mid << "\r\n";
            media_answers[media_index] = media_answer.str();
            continue;
        }

        accepted_mids.push_back(media.mid);
        if (lower_copy(media.type) == "video")
        {
            video_codec = config.video.codec == output_video_codec::av1 ? codec_id::av1 : video_track->codec;
            video_payload_type = codec->payload_type;
            video_mid = media.mid;
            video_mid_extension_id = media.mid_extension_id;
            bundle_mid_extension_id = media.mid_extension_id;
            spdlog::debug("webrtc answer video mid {} pt {} codec {}", media.mid, codec->payload_type, to_string(*video_codec));
        }
        else
        {
            audio_codec = audio_track->codec;
            audio_payload_type = codec->payload_type;
            audio_mid = media.mid;
            audio_mid_extension_id = media.mid_extension_id;
            bundle_mid_extension_id = media.mid_extension_id;
            audio_channel_count = audio_track->codec == codec_id::g711a || audio_track->codec == codec_id::g711u
                ? 1
                : opus_output_channel_count(*codec, *audio_track);
            if (audio_track->codec == codec_id::aac || audio_track->codec == codec_id::opus)
            {
                audio_bitrate = 64'000 * *audio_channel_count;
                const auto offered_bitrate = decimal_parameter(codec->format_parameters, "maxaveragebitrate", -1, 510'000);
                if (offered_bitrate && *offered_bitrate >= 0)
                {
                    audio_bitrate = std::min(*audio_bitrate, std::max(*offered_bitrate, 6'000));
                }
                const auto offered_max_playback_rate = decimal_parameter(codec->format_parameters, "maxplaybackrate", 48'000, 48'000);
                audio_max_playback_rate =
                    offered_max_playback_rate && *offered_max_playback_rate >= 8'000 ? *offered_max_playback_rate : 48'000;
            }
            spdlog::debug("webrtc answer audio mid {} pt {} codec {} channels {} bitrate {} max_playback_rate {}",
                          media.mid,
                          codec->payload_type,
                          to_string(*audio_codec),
                          *audio_channel_count,
                          audio_bitrate.value_or(0),
                          audio_max_playback_rate.value_or(0));
        }
        media_answer << "m=" << media.type << ' ' << config.port << ' ' << media.protocol << ' ' << codec->payload_type << "\r\n";
        media_answer << "c=IN " << address_type << ' ' << config.address.to_string() << "\r\n";
        media_answer << "a=mid:" << media.mid << "\r\n";
        media_answer << "a=extmap:" << *media.mid_extension_id << ' ' << mid_extension_uri << "\r\n";
        media_answer << "a=sendonly\r\n";
        media_answer << "a=msid:" << config.stream_id << "\r\n";

        if (lower_copy(media.type) == "video")
        {
            if (config.video.codec == output_video_codec::av1)
            {
                media_answer << "a=rtpmap:" << codec->payload_type << " AV1/90000\r\n";
                if (!codec->format_parameters.empty())
                {
                    media_answer << "a=fmtp:" << codec->payload_type << ' ' << codec->format_parameters << "\r\n";
                }
            }
            else if (video_track->codec == codec_id::h264)
            {
                media_answer << "a=rtpmap:" << codec->payload_type << " H264/90000\r\n";
                media_answer << "a=fmtp:" << codec->payload_type
                             << " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=" << profile_level_id << "\r\n";
            }
            else
            {
                const auto source = source_h265_profile_tier_level(*video_track);
                if (!source)
                {
                    return std::nullopt;
                }
                media_answer << "a=rtpmap:" << codec->payload_type << " H265/90000\r\n";
                media_answer << "a=fmtp:" << codec->payload_type << " profile-space=" << source->profile_space << ";profile-id=" << source->profile
                             << ";tier-flag=" << source->tier << ";level-id=" << source->level << "\r\n";
            }
        }
        else
        {
            if (audio_track->codec == codec_id::aac || audio_track->codec == codec_id::opus)
            {
                media_answer << "a=rtpmap:" << codec->payload_type << " opus/48000/2\r\n";
                media_answer << "a=fmtp:" << codec->payload_type << " minptime=10;useinbandfec=1;sprop-stereo="
                             << (*audio_channel_count == 2 ? 1 : 0) << "\r\n";
            }
        }
        media_answers[media_index] = media_answer.str();
    }

    if (accepted_mids.empty())
    {
        return std::nullopt;
    }

    std::ostringstream answer;
    answer << "v=0\r\n";
    answer << "o=- 1 1 IN " << address_type << ' ' << config.address.to_string() << "\r\n";
    answer << "s=media_server\r\n";
    answer << "t=0 0\r\n";
    answer << "a=ice-lite\r\n";
    answer << "a=group:BUNDLE " << transport_mid;
    for (const auto& mid : offer.bundle_mids)
    {
        if (mid != transport_mid && std::find(accepted_mids.begin(), accepted_mids.end(), mid) != accepted_mids.end())
        {
            answer << ' ' << mid;
        }
    }
    answer << "\r\n";
    for (std::size_t index = 0; index < offer.media.size(); ++index)
    {
        const auto& mid = offer.media[index].mid;
        answer << media_answers[index];
        // libwebrtc 会逐个校验 bundled RTP media 是否显式启用 rtcp-mux。
        if (std::find(accepted_mids.begin(), accepted_mids.end(), mid) != accepted_mids.end())
        {
            answer << "a=rtcp-mux\r\n";
        }
        if (mid == transport_mid)
        {
            append_transport(answer, config);
        }
    }

    return webrtc_answer{
        .sdp = answer.str(),
        .transport_mid = transport_mid,
        .video_codec = video_codec,
        .audio_codec = audio_codec,
        .video_payload_type = video_payload_type,
        .audio_payload_type = audio_payload_type,
        .video_mid = video_mid,
        .audio_mid = audio_mid,
        .video_mid_extension_id = video_mid_extension_id,
        .audio_mid_extension_id = audio_mid_extension_id,
        .audio_channel_count = audio_channel_count,
        .audio_bitrate = audio_bitrate,
        .audio_max_playback_rate = audio_max_playback_rate,
    };
}

}    // namespace media_server
