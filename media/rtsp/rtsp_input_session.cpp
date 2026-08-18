#include "media/rtsp/rtsp_input_session.h"

#include "media/codec/codec_utils.h"
#include "media/rtsp/rtsp_input_tcp_session.h"
#include "media/rtsp/rtsp_input_udp_session.h"

extern "C"
{
#include "base64.h"
#include "mpeg4-aac.h"
#include "rtp-profile.h"
#include "sdp-a-fmtp.h"
#include "sdp.h"
}

#include <boost/url/parse.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <string>
#include <utility>

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

bool iequals(const char* value, std::string_view expected)
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

bool append_sdp_parameter_sets(std::vector<std::uint8_t>& config, std::string_view encoded)
{
    constexpr std::array<std::uint8_t, 4> start_code{0, 0, 0, 1};
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

std::optional<std::uint16_t> opus_channels(const char* fmtp)
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
            if (name.size() == std::string_view("sprop-stereo").size() &&
                std::equal(name.begin(),
                           name.end(),
                           std::string_view("sprop-stereo").begin(),
                           [](unsigned char left, unsigned char right) { return std::tolower(left) == std::tolower(right); }))
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

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

}    // namespace

rtsp_input_session::rtsp_input_session(std::weak_ptr<rtsp_server_connection> connection,
                                       stream_registry& registry,
                                       std::vector<std::uint8_t> initial_data)
    : connection_(std::move(connection)), registry_(registry), initial_data_(std::move(initial_data))
{
}

void rtsp_input_session::startup()
{
    const auto connection = connection_.lock();
    if (closed_ || !connection)
    {
        return;
    }

    const auto self = shared_from_this();
    auto handler = std::make_shared<rtsp_server_connection_handler>();
    handler->on_read = [self](std::span<const std::uint8_t> data) { return self->on_read(data); };
    handler->on_shutdown = [self]() { self->safe_shutdown(); };
    handler->on_setup = [self](rtsp_server_t* server,
                               const char* uri,
                               const char* session,
                               const rtsp_header_transport_t transports[],
                               std::size_t count)
    { return self->on_setup(server, uri, session, transports, count); };
    handler->on_teardown = [self](rtsp_server_t* server, const char*, const char* session) { return self->on_teardown(server, session); };
    handler->on_announce = [self](rtsp_server_t* server, const char* uri, const char* sdp, int length)
    { return self->on_announce(server, uri, sdp, length); };
    handler->on_record = [self](rtsp_server_t* server, const char*, const char* session, const std::int64_t*, const double*)
    { return self->on_record(server, session); };
    handler->on_get_parameter = [](rtsp_server_t* server, const char*, const char*, const void*, int)
    { return rtsp_server_reply_get_parameter(server, 200, nullptr, 0); };

    if (!connection->startup(std::move(handler), std::move(initial_data_)))
    {
        connection->shutdown();
    }
    initial_data_.clear();
}

void rtsp_input_session::shutdown()
{
    if (const auto connection = connection_.lock())
    {
        connection->shutdown();
    }
}

std::size_t rtsp_input_session::on_read(std::span<const std::uint8_t> data)
{
    const auto connection = connection_.lock();
    return connection ? connection->input(data) : data.size();
}

int rtsp_input_session::on_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length)
{
    if (announced_ || sdp == nullptr || length <= 0)
    {
        return rtsp_server_reply_announce(server, 455);
    }

    descriptions_.clear();
    stream_name_ = stream_name_from_uri(uri);
    if (stream_name_.empty())
    {
        return rtsp_server_reply_announce(server, 400);
    }

    const auto count = rtsp_media_sdp(sdp, length, nullptr, 0);
    if (count <= 0)
    {
        return rtsp_server_reply_announce(server, 415);
    }
    std::vector<rtsp_media_t> media(static_cast<std::size_t>(count));
    if (rtsp_media_sdp(sdp, length, media.data(), count) != count)
    {
        return rtsp_server_reply_announce(server, 415);
    }

    const auto* content_base = rtsp_server_get_header(server, "Content-Base");
    const auto* content_location = rtsp_server_get_header(server, "Content-Location");
    for (auto& description : media)
    {
        if (rtsp_media_set_url(&description, content_base, content_location, std::string(uri).c_str()) != 0)
        {
            return rtsp_server_reply_announce(server, 400);
        }
    }

    bool video = false;
    bool audio = false;
    for (const auto& description : media)
    {
        std::optional<rtsp_input_track_description> selected;
        for (int format_index = 0; format_index < description.avformat_count; ++format_index)
        {
            const auto& format = description.avformats[static_cast<std::size_t>(format_index)];
            auto track = track_from_format(description, format);
            if (!track)
            {
                continue;
            }
            selected = rtsp_input_track_description{
                .uri = description.uri,
                .track = std::move(*track),
                .clock_rate = format.rate,
                .payload_type = format.fmt,
                .encoding = format.encoding,
                .fmtp = format.fmtp != nullptr ? format.fmtp : "",
            };
            break;
        }
        if (!selected)
        {
            continue;
        }
        if ((selected->track.kind == media_kind::video && video) || (selected->track.kind == media_kind::audio && audio) || descriptions_.size() >= 2)
        {
            descriptions_.clear();
            return rtsp_server_reply_announce(server, 415);
        }
        if (selected->track.kind == media_kind::video)
        {
            video = true;
        }
        else
        {
            audio = true;
        }
        descriptions_.push_back(std::move(*selected));
    }
    if (!video)
    {
        descriptions_.clear();
        return rtsp_server_reply_announce(server, 415);
    }

    announced_ = true;
    session_id_ = std::to_string(random_u32());
    return rtsp_server_reply_announce(server, 200);
}

int rtsp_input_session::on_setup(rtsp_server_t* server,
                                 std::string_view uri,
                                 std::string_view session,
                                 const rtsp_header_transport_t transports[],
                                 std::size_t count)
{
    if (!announced_ || transports == nullptr || count == 0 || (!session.empty() && session != session_id_))
    {
        return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
    }

    bool known_uri = false;
    for (const auto& description : descriptions_)
    {
        if (uri == description.uri)
        {
            known_uri = true;
            break;
        }
    }
    if (!known_uri)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }

    const rtsp_header_transport_t* selected = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        const bool tcp = transports[index].transport == RTSP_TRANSPORT_RTP_TCP;
        const bool udp = transports[index].transport == RTSP_TRANSPORT_RTP_UDP;
        const bool valid_interleaved = transports[index].interleaved1 >= 0 && transports[index].interleaved2 >= 0 &&
                                       transports[index].interleaved1 <= 255 && transports[index].interleaved2 <= 255 &&
                                       transports[index].interleaved1 != transports[index].interleaved2;
        if ((tcp || udp) && transports[index].multicast == 0 &&
            (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_RECORD) && (!tcp || valid_interleaved))
        {
            selected = &transports[index];
            break;
        }
    }
    if (selected == nullptr)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }

    if (selected->transport == RTSP_TRANSPORT_RTP_TCP)
    {
        auto child = std::make_shared<rtsp_input_tcp_session>(connection_, connection->executor(), registry_, stream_name_, session_id_, descriptions_);
        return child->startup(server, uri, session, transports, count);
    }

    auto child = std::make_shared<rtsp_input_udp_session>(connection_, connection->executor(), registry_, stream_name_, session_id_, descriptions_);
    return child->startup(server, uri, session, transports, count);
}

int rtsp_input_session::on_record(rtsp_server_t* server, std::string_view session)
{
    if (!announced_ || session != session_id_)
    {
        return rtsp_server_reply_record(server, 454, nullptr, nullptr);
    }
    return rtsp_server_reply_record(server, 455, nullptr, nullptr);
}

int rtsp_input_session::on_teardown(rtsp_server_t* server, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    shutdown();
    return result;
}

void rtsp_input_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    descriptions_.clear();
}

std::optional<media_track> rtsp_input_session::track_from_format(const rtsp_media_t& media, const rtsp_media_t::avformat_t& format)
{
    const auto video = iequals(media.media, "video");
    const auto audio = iequals(media.media, "audio");
    if (video && iequals(format.encoding, "H264"))
    {
        sdp_a_fmtp_h264_t parameters{};
        auto payload = format.fmt;
        std::vector<std::uint8_t> config;
        if (format.fmtp != nullptr && sdp_a_fmtp_h264(format.fmtp, &payload, &parameters) == 0 &&
            append_sdp_parameter_sets(config, parameters.sprop_parameter_sets) && !h264_annex_b_to_avcc(config).empty())
        {
            return media_track{.id = video_track_id, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (video && (iequals(format.encoding, "H265") || iequals(format.encoding, "HEVC")))
    {
        sdp_a_fmtp_h265_t parameters{};
        auto payload = format.fmt;
        std::vector<std::uint8_t> config;
        if (format.fmtp != nullptr && sdp_a_fmtp_h265(format.fmtp, &payload, &parameters) == 0 &&
            append_sdp_parameter_sets(config, parameters.sprop_vps) && append_sdp_parameter_sets(config, parameters.sprop_sps) &&
            append_sdp_parameter_sets(config, parameters.sprop_pps) && !h265_annex_b_to_hvcc(config).empty())
        {
            return media_track{.id = video_track_id, .kind = media_kind::video, .codec = codec_id::h265, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (audio && iequals(format.encoding, "MPEG4-GENERIC"))
    {
        sdp_a_fmtp_mpeg4_t parameters{};
        auto payload = format.fmt;
        if (format.fmtp != nullptr && sdp_a_fmtp_mpeg4(format.fmtp, &payload, &parameters) == 0)
        {
            const std::string_view encoded(parameters.config);
            if (!encoded.empty() && encoded.size() % 2U == 0U &&
                std::all_of(encoded.begin(), encoded.end(), [](char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; }))
            {
                std::vector<std::uint8_t> config(encoded.size() / 2U);
                if (base16_decode(config.data(), encoded.data(), encoded.size()) > 0)
                {
                    if (const auto aac = parse_aac_asc(config))
                    {
                        return media_track{.id = audio_track_id,
                                           .kind = media_kind::audio,
                                           .codec = codec_id::aac,
                                           .clock_rate = aac->sample_rate,
                                           .channel_count = aac->channel_count,
                                           .codec_config = std::move(config)};
                    }
                }
            }
        }
    }
    else if (audio && iequals(format.encoding, "opus") && format.rate == 48'000)
    {
        if (const auto channels = opus_channels(format.fmtp))
        {
            return media_track{.id = audio_track_id,
                               .kind = media_kind::audio,
                               .codec = codec_id::opus,
                               .clock_rate = 48'000,
                               .channel_count = *channels,
                               .codec_config = {}};
        }
    }
    else if (audio && format.rate == 8'000 &&
             ((format.fmt == RTP_PAYLOAD_PCMA && (format.encoding[0] == '\0' || iequals(format.encoding, "PCMA"))) ||
              (format.fmt == RTP_PAYLOAD_PCMU && (format.encoding[0] == '\0' || iequals(format.encoding, "PCMU")))))
    {
        return media_track{.id = audio_track_id,
                           .kind = media_kind::audio,
                           .codec = format.fmt == RTP_PAYLOAD_PCMA ? codec_id::g711a : codec_id::g711u,
                           .clock_rate = 8'000,
                           .channel_count = 1,
                           .codec_config = {}};
    }
    return std::nullopt;
}

std::string rtsp_input_session::stream_name_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return {};
    }
    std::string result;
    for (const auto segment : parsed->segments())
    {
        const std::string value(segment);
        if (value.starts_with("trackID="))
        {
            break;
        }
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(value);
    }
    return result;
}

}    // namespace media_server
