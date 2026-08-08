#include "media/webrtc/webrtc_sdp.h"

extern "C"
{
#include "sdp-a-rtpmap.h"
#include "sdp.h"
}

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>

namespace media_server
{
namespace
{

struct sdp_deleter
{
    void operator()(sdp_t* value) const noexcept
    {
        sdp_destroy(value);
    }
};

using sdp_ptr = std::unique_ptr<sdp_t, sdp_deleter>;

std::string lower_copy(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

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
    media->codecs.push_back(webrtc_codec_offer{
        .payload_type = payload_type,
        .encoding_name = encoding,
        .clock_rate = static_cast<std::uint32_t>(clock_rate),
        .channel_count = channel_count,
        .format_parameters = {},
    });
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
    const auto iterator = std::find_if(media->codecs.begin(), media->codecs.end(), [payload_type](const webrtc_codec_offer& codec) {
        return codec.payload_type == payload_type;
    });
    if (iterator != media->codecs.end())
    {
        iterator->format_parameters = std::string(text.substr(space + 1));
    }
}

bool has_parameter(std::string_view parameters, std::string_view name, std::string_view value)
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
        if (equal != std::string_view::npos && lower_copy(item.substr(0, equal)) == lower_copy(name) && item.substr(equal + 1) == value)
        {
            return true;
        }
        begin = end + 1;
    }
    return false;
}

const media_track* find_track(const std::vector<media_track>& tracks, media_kind kind, codec_id codec)
{
    const auto iterator = std::find_if(tracks.begin(), tracks.end(), [kind, codec](const media_track& track) {
        return track.kind == kind && track.codec == codec;
    });
    return iterator == tracks.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_h264(const webrtc_media_offer& media)
{
    const auto iterator = std::find_if(media.codecs.begin(), media.codecs.end(), [](const webrtc_codec_offer& codec) {
        return lower_copy(codec.encoding_name) == "h264" && codec.clock_rate == 90'000U && has_parameter(codec.format_parameters, "packetization-mode", "1");
    });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
}

const webrtc_codec_offer* find_opus(const webrtc_media_offer& media)
{
    const auto iterator = std::find_if(media.codecs.begin(), media.codecs.end(), [](const webrtc_codec_offer& codec) {
        return lower_copy(codec.encoding_name) == "opus" && codec.clock_rate == 48'000U && (codec.channel_count == 0 || codec.channel_count == 2);
    });
    return iterator == media.codecs.end() ? nullptr : &*iterator;
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
        stream << std::hex << std::setfill('0') << std::nouppercase
               << std::setw(2) << static_cast<unsigned int>(data[nalu + 1U])
               << std::setw(2) << static_cast<unsigned int>(data[nalu + 2U])
               << std::setw(2) << static_cast<unsigned int>(data[nalu + 3U]);
        return stream.str();
    }
    return {};
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
    if (const char* group = sdp_attribute_find(sdp.get(), "group"); group != nullptr)
    {
        const auto fields = split_words(group);
        if (!fields.empty() && lower_copy(fields.front()) == "bundle")
        {
            for (std::size_t index = 1; index < fields.size(); ++index)
            {
                result.bundle_mids.emplace_back(fields[index]);
            }
        }
    }

    const char* session_ice_ufrag = sdp_attribute_find(sdp.get(), "ice-ufrag");
    const char* session_ice_pwd = sdp_attribute_find(sdp.get(), "ice-pwd");
    const char* session_fingerprint = sdp_attribute_find(sdp.get(), "fingerprint");

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
        if (type == nullptr || protocol == nullptr || mid == nullptr || *mid == '\0')
        {
            return std::nullopt;
        }

        webrtc_media_offer media{
            .type = type,
            .protocol = protocol,
            .mid = mid,
            .direction = direction(sdp_media_mode(sdp.get(), index)),
            .ice_ufrag = attribute(sdp.get(), index, "ice-ufrag", session_ice_ufrag),
            .ice_pwd = attribute(sdp.get(), index, "ice-pwd", session_ice_pwd),
            .fingerprint = attribute(sdp.get(), index, "fingerprint", session_fingerprint),
            .rtcp_mux = sdp_media_attribute_find(sdp.get(), index, "rtcp-mux") != nullptr,
            .payload_types = {},
            .codecs = {},
        };

        const auto format_count = sdp_media_formats(sdp.get(), index, nullptr, 0);
        if (format_count > 0)
        {
            media.payload_types.resize(static_cast<std::size_t>(format_count));
            static_cast<void>(sdp_media_formats(sdp.get(), index, media.payload_types.data(), format_count));
        }

        static_cast<void>(sdp_media_attribute_list(sdp.get(), index, "rtpmap", &on_rtpmap, &media));
        static_cast<void>(sdp_media_attribute_list(sdp.get(), index, "fmtp", &on_fmtp, &media));
        result.media.push_back(std::move(media));
    }

    return result;
}

std::optional<std::string> make_webrtc_answer(
    const webrtc_offer& offer,
    const std::vector<media_track>& tracks,
    const webrtc_answer_config& config)
{
    if (config.port == 0 || config.ice_ufrag.empty() || config.ice_pwd.empty() || config.fingerprint.empty())
    {
        return std::nullopt;
    }

    const auto* video_track = find_track(tracks, media_kind::video, codec_id::h264);
    const auto* audio_track = find_track(tracks, media_kind::audio, codec_id::aac);
    const auto address_type = config.address.is_v6() ? "IP6" : "IP4";

    std::ostringstream answer;
    answer << "v=0\r\n";
    answer << "o=- 1 1 IN " << address_type << ' ' << config.address.to_string() << "\r\n";
    answer << "s=media_server\r\n";
    answer << "t=0 0\r\n";
    answer << "a=ice-lite\r\n";

    if (!offer.bundle_mids.empty())
    {
        answer << "a=group:BUNDLE";
        for (const auto& mid : offer.bundle_mids)
        {
            answer << ' ' << mid;
        }
        answer << "\r\n";
    }

    bool accepted_any = false;
    for (const auto& media : offer.media)
    {
        const auto media_direction = lower_copy(media.direction);
        const bool can_receive = media_direction == "sendrecv" || media_direction == "recvonly";
        const webrtc_codec_offer* codec = nullptr;
        std::string profile_level_id;

        if (can_receive && lower_copy(media.type) == "video" && video_track != nullptr)
        {
            codec = find_h264(media);
            profile_level_id = h264_profile_level_id(*video_track);
            if (profile_level_id.empty())
            {
                codec = nullptr;
            }
        }
        else if (can_receive && lower_copy(media.type) == "audio" && audio_track != nullptr)
        {
            codec = find_opus(media);
        }

        if (codec == nullptr)
        {
            answer << "m=" << media.type << " 0 " << media.protocol;
            for (const auto payload_type : media.payload_types)
            {
                answer << ' ' << payload_type;
            }
            answer << "\r\n";
            answer << "c=IN " << address_type << ' ' << (config.address.is_v6() ? "::" : "0.0.0.0") << "\r\n";
            answer << "a=mid:" << media.mid << "\r\n";
            answer << "a=inactive\r\n";
            continue;
        }

        accepted_any = true;
        answer << "m=" << media.type << ' ' << config.port << ' ' << media.protocol << ' ' << codec->payload_type << "\r\n";
        answer << "c=IN " << address_type << ' ' << config.address.to_string() << "\r\n";
        answer << "a=mid:" << media.mid << "\r\n";
        answer << "a=sendonly\r\n";
        if (media.rtcp_mux)
        {
            answer << "a=rtcp-mux\r\n";
        }
        append_transport(answer, config);

        if (lower_copy(media.type) == "video")
        {
            answer << "a=rtpmap:" << codec->payload_type << " H264/90000\r\n";
            answer << "a=fmtp:" << codec->payload_type
                   << " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=" << profile_level_id << "\r\n";
        }
        else
        {
            answer << "a=rtpmap:" << codec->payload_type << " opus/48000/2\r\n";
            answer << "a=fmtp:" << codec->payload_type << " minptime=10;useinbandfec=1\r\n";
        }
    }

    if (!accepted_any)
    {
        return std::nullopt;
    }
    return answer.str();
}

}    // namespace media_server
