#include "media/rtsp/rtsp_publish_session.h"

#include "media/codec/codec_utils.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "avpacket.h"
#include "base64.h"
#include "mpeg4-aac.h"
#include "rtp-profile.h"
#include "rtsp-demuxer.h"
#include "sdp-a-fmtp.h"
#include "sdp.h"
}

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <random>
#include <string_view>
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

rtsp_publish_session::rtsp_publish_session(std::shared_ptr<tcp_connection> connection,
                                           stream_registry& registry,
                                           std::vector<std::uint8_t> initial_data)
    : connection_(std::move(connection)), registry_(registry), initial_data_(std::move(initial_data))
{
}

rtsp_publish_session::~rtsp_publish_session()
{
    if (interleaved_.data != nullptr)
    {
        std::free(interleaved_.data);
        interleaved_.data = nullptr;
    }
    reset_media();
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
    }
    avpkt2bs_destroy(&bitstream_);
}

void rtsp_publish_session::startup()
{
    if (closed_ || connection_ == nullptr)
    {
        return;
    }

    rtsp_handler_t handler{};
    handler.send = &rtsp_publish_session::send_callback;
    handler.onannounce = &rtsp_publish_session::announce_callback;
    handler.onsetup = &rtsp_publish_session::setup_callback;
    handler.onrecord = &rtsp_publish_session::record_callback;
    handler.onoptions = &rtsp_publish_session::options_callback;
    handler.onteardown = &rtsp_publish_session::teardown_callback;
    handler.ongetparameter = &rtsp_publish_session::get_parameter_callback;

    boost::system::error_code error;
    const auto peer = connection_->socket().remote_endpoint(error);
    const auto address = error ? std::string("0.0.0.0") : peer.address().to_string();
    server_ = rtsp_server_create(address.c_str(), error ? 0 : peer.port(), &handler, this, this);
    if (server_ == nullptr)
    {
        shutdown();
        return;
    }

    interleaved_.onrtp = &rtsp_publish_session::rtp_callback;
    interleaved_.param = this;
    static_cast<void>(avpkt2bs_create(&bitstream_));

    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code read_error, std::span<const std::uint8_t> data)
        {
            if (read_error)
            {
                self->shutdown();
                return;
            }
            self->on_tcp_read(data);
        },
        [self](boost::system::error_code write_error, std::size_t)
        {
            if (write_error)
            {
                self->shutdown();
            }
        });

    if (!initial_data_.empty())
    {
        const auto initial = std::move(initial_data_);
        initial_data_.clear();
        on_tcp_read(initial);
    }
}

void rtsp_publish_session::shutdown()
{
    if (closed_ || connection_ == nullptr)
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(), [self]() { self->safe_shutdown(); });
}

int rtsp_publish_session::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_publish_session*>(param);
    if (self->closed_ || self->connection_ == nullptr)
    {
        return -1;
    }
    self->connection_->write(data, bytes);
    return static_cast<int>(bytes);
}

int rtsp_publish_session::announce_callback(void* param, rtsp_server_t*, const char* uri, const char* sdp, int length)
{
    return static_cast<rtsp_publish_session*>(param)->on_announce(uri != nullptr ? uri : "", sdp, length);
}

int rtsp_publish_session::setup_callback(void* param,
                                         rtsp_server_t*,
                                         const char* uri,
                                         const char* session,
                                         const rtsp_header_transport_t transports[],
                                         std::size_t count)
{
    return static_cast<rtsp_publish_session*>(param)->on_setup(
        uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int rtsp_publish_session::record_callback(void* param,
                                          rtsp_server_t*,
                                          const char* uri,
                                          const char* session,
                                          const std::int64_t*,
                                          const double*)
{
    return static_cast<rtsp_publish_session*>(param)->on_record(uri != nullptr ? uri : "", session != nullptr ? session : "");
}

int rtsp_publish_session::options_callback(void*, rtsp_server_t* server, const char*)
{
    return rtsp_server_reply_options(server, 200);
}

int rtsp_publish_session::teardown_callback(void* param, rtsp_server_t* server, const char*, const char* session)
{
    auto* self = static_cast<rtsp_publish_session*>(param);
    if (session == nullptr || self->session_id_ != session)
    {
        return rtsp_server_reply_teardown(server, 454);
    }
    const auto result = rtsp_server_reply_teardown(server, 200);
    self->shutdown();
    return result;
}

int rtsp_publish_session::get_parameter_callback(void*, rtsp_server_t* server, const char*, const char*, const void*, int)
{
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

void rtsp_publish_session::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    static_cast<rtsp_publish_session*>(param)->on_rtp(channel, data, bytes);
}

int rtsp_publish_session::packet_callback(void* param, avpacket_t* packet)
{
    return static_cast<rtsp_publish_session*>(param)->on_packet(packet);
}

void rtsp_publish_session::on_tcp_read(std::span<const std::uint8_t> data)
{
    if (closed_ || server_ == nullptr)
    {
        return;
    }

    auto remaining = data;
    while (!remaining.empty() && !closed_)
    {
        if (interleaved_.state != 0 || remaining.front() == '$')
        {
            const auto* next = rtp_over_rtsp(&interleaved_, remaining.data(), remaining.data() + remaining.size());
            if (next == remaining.data())
            {
                shutdown();
                return;
            }
            remaining = std::span(next, remaining.data() + remaining.size());
            if (interleaved_.state != 0)
            {
                return;
            }
            continue;
        }

        auto bytes = remaining.size();
        const auto result = rtsp_server_input(server_, remaining.data(), &bytes);
        if (result < 0)
        {
            shutdown();
            return;
        }
        if (result > 0 || bytes == 0)
        {
            return;
        }
        const auto consumed = remaining.size() - bytes;
        if (consumed == 0)
        {
            shutdown();
            return;
        }
        remaining = remaining.subspan(consumed);
    }
}

void rtsp_publish_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    if (!recording_ || data == nullptr || bytes < 12)
    {
        return;
    }
    for (auto& state : media_)
    {
        if (state.demuxer != nullptr && state.rtp_channel == channel)
        {
            static_cast<void>(rtsp_demuxer_input(state.demuxer, data, static_cast<int>(bytes)));
            return;
        }
    }
}

void rtsp_publish_session::on_udp_rtp(std::size_t media_index,
                                      std::span<const std::uint8_t> data,
                                      const boost::asio::ip::udp::endpoint& endpoint)
{
    if (!recording_ || media_index >= media_count_ || data.size() < 12)
    {
        return;
    }
    auto& state = media_[media_index];
    if (endpoint != state.client_endpoint)
    {
        return;
    }
    if (state.demuxer != nullptr)
    {
        static_cast<void>(rtsp_demuxer_input(state.demuxer, data.data(), static_cast<int>(data.size())));
    }
}

std::optional<media_track> rtsp_publish_session::track_from_format(const rtsp_media_t& media, const rtsp_media_t::avformat_t& format)
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

std::string rtsp_publish_session::stream_name_from_uri(std::string_view uri)
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

int rtsp_publish_session::on_announce(std::string_view uri, const char* sdp, int length)
{
    if (announced_ || sdp == nullptr || length <= 0)
    {
        return rtsp_server_reply_announce(server_, 455);
    }

    reset_media();
    stream_name_ = stream_name_from_uri(uri);
    if (stream_name_.empty())
    {
        return rtsp_server_reply_announce(server_, 400);
    }

    const auto count = rtsp_media_sdp(sdp, length, nullptr, 0);
    if (count <= 0)
    {
        return rtsp_server_reply_announce(server_, 415);
    }
    media_descriptions_.resize(static_cast<std::size_t>(count));
    if (rtsp_media_sdp(sdp, length, media_descriptions_.data(), count) != count)
    {
        reset_media();
        return rtsp_server_reply_announce(server_, 415);
    }

    const auto* content_base = rtsp_server_get_header(server_, "Content-Base");
    const auto* content_location = rtsp_server_get_header(server_, "Content-Location");
    for (int index = 0; index < count; ++index)
    {
        if (rtsp_media_set_url(&media_descriptions_[static_cast<std::size_t>(index)], content_base, content_location, std::string(uri).c_str()) != 0)
        {
            reset_media();
            return rtsp_server_reply_announce(server_, 400);
        }
    }

    bool video = false;
    bool audio = false;
    media_count_ = 0;
    for (int index = 0; index < count; ++index)
    {
        const auto& description = media_descriptions_[static_cast<std::size_t>(index)];
        std::optional<media_track> track;
        for (int format_index = 0; format_index < description.avformat_count; ++format_index)
        {
            track = track_from_format(description, description.avformats[static_cast<std::size_t>(format_index)]);
            if (track)
            {
                break;
            }
        }
        if (!track)
        {
            continue;
        }
        if ((track->kind == media_kind::video && video) || (track->kind == media_kind::audio && audio) || media_count_ >= media_.size())
        {
            return rtsp_server_reply_announce(server_, 415);
        }
        if (track->kind == media_kind::video)
        {
            video = true;
        }
        else
        {
            audio = true;
        }
        media_[media_count_].media_index = static_cast<std::size_t>(index);
        media_[media_count_].track = std::move(track);
        ++media_count_;
    }
    if (!video)
    {
        return rtsp_server_reply_announce(server_, 415);
    }

    stream_ = std::make_shared<media_stream>(stream_name_, connection_->socket().get_executor());
    for (std::size_t index = 0; index < media_count_; ++index)
    {
        const auto& description = media_descriptions_[media_[index].media_index];
        const auto& track = *media_[index].track;
        const auto format = std::find_if(description.avformats,
                                         description.avformats + description.avformat_count,
                                         [&track, &description](const rtsp_media_t::avformat_t& value)
                                         {
                                             return rtsp_publish_session::track_from_format(description, value).has_value() &&
                                                    ((track.codec == codec_id::h264 && iequals(value.encoding, "H264")) ||
                                                     (track.codec == codec_id::h265 && (iequals(value.encoding, "H265") || iequals(value.encoding, "HEVC"))) ||
                                                     (track.codec == codec_id::aac && iequals(value.encoding, "MPEG4-GENERIC")) ||
                                                     (track.codec == codec_id::opus && iequals(value.encoding, "opus")) ||
                                                     (track.codec == codec_id::g711a && value.fmt == RTP_PAYLOAD_PCMA) ||
                                                     (track.codec == codec_id::g711u && value.fmt == RTP_PAYLOAD_PCMU));
                                         });
        if (format == description.avformats + description.avformat_count)
        {
            reset_media();
            return rtsp_server_reply_announce(server_, 415);
        }
        media_[index].demuxer = rtsp_demuxer_create(static_cast<int>(index), 500, &rtsp_publish_session::packet_callback, this);
        if (media_[index].demuxer == nullptr ||
            rtsp_demuxer_add_payload(media_[index].demuxer, format->rate, format->fmt, format->encoding, format->fmtp) != 0)
        {
            reset_media();
            return rtsp_server_reply_announce(server_, 415);
        }
    }

    announced_ = true;
    session_id_ = std::to_string(random_u32());
    return rtsp_server_reply_announce(server_, 200);
}

int rtsp_publish_session::on_setup(std::string_view uri,
                                   std::string_view session,
                                   const rtsp_header_transport_t transports[],
                                   std::size_t count)
{
    if (!announced_ || transports == nullptr || count == 0 || (!session.empty() && session != session_id_))
    {
        return rtsp_server_reply_setup(server_, 454, nullptr, nullptr);
    }

    std::size_t selected = media_count_;
    for (std::size_t index = 0; index < media_count_; ++index)
    {
        const auto& description = media_descriptions_[media_[index].media_index];
        if (uri == description.uri)
        {
            selected = index;
            break;
        }
    }
    if (selected == media_count_ || media_[selected].setup)
    {
        return rtsp_server_reply_setup(server_, 404, nullptr, nullptr);
    }

    const rtsp_header_transport_t* transport = nullptr;
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
            transport = &transports[index];
            break;
        }
    }
    if (transport == nullptr)
    {
        return rtsp_server_reply_setup(server_, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }
    if (transport->transport == RTSP_TRANSPORT_RTP_TCP)
    {
        for (const auto& state : media_)
        {
            if (state.setup && (state.rtp_channel == transport->interleaved1 || state.rtp_channel == transport->interleaved2 ||
                                state.rtcp_channel == transport->interleaved1 || state.rtcp_channel == transport->interleaved2))
            {
                return rtsp_server_reply_setup(server_, 461, nullptr, nullptr);
            }
        }
    }

    auto& state = media_[selected];
    if (transport->transport == RTSP_TRANSPORT_RTP_UDP)
    {
        if (transport->rtp.u.client_port1 == 0 || transport->rtp.u.client_port2 == 0)
        {
            return rtsp_server_reply_setup(server_, 461, nullptr, nullptr);
        }
        boost::system::error_code address_error;
        const auto client_address = boost::asio::ip::make_address(rtsp_server_get_client(server_, nullptr), address_error);
        if (address_error)
        {
            return rtsp_server_reply_setup(server_, 461, nullptr, nullptr);
        }
        const auto self = shared_from_this();
        std::shared_ptr<udp_socket> rtp_socket;
        std::shared_ptr<udp_socket> rtcp_socket;
        for (std::uint32_t port = 49'152; port <= 65'534; port += 2U)
        {
            auto candidate_rtp = std::make_shared<udp_socket>(connection_->socket().get_executor());
            if (!candidate_rtp->startup(boost::asio::ip::address_v4::any(),
                                        static_cast<std::uint16_t>(port),
                                        [self, selected](boost::system::error_code error,
                                                         std::span<const std::uint8_t> data,
                                                         const boost::asio::ip::udp::endpoint& endpoint)
                                        {
                                            if (!error)
                                            {
                                                self->on_udp_rtp(selected, data, endpoint);
                                            }
                                        },
                                        [self](boost::system::error_code error, const boost::asio::ip::udp::endpoint&)
                                        {
                                            if (error)
                                            {
                                                self->shutdown();
                                            }
                                        }))
            {
                continue;
            }
            auto candidate_rtcp = std::make_shared<udp_socket>(connection_->socket().get_executor());
            if (!candidate_rtcp->startup(boost::asio::ip::address_v4::any(),
                                         static_cast<std::uint16_t>(port + 1U),
                                         [](boost::system::error_code, std::span<const std::uint8_t>,
                                            const boost::asio::ip::udp::endpoint&) {},
                                         [](boost::system::error_code, const boost::asio::ip::udp::endpoint&) {}))
            {
                candidate_rtp->shutdown();
                continue;
            }
            rtp_socket = std::move(candidate_rtp);
            rtcp_socket = std::move(candidate_rtcp);
            break;
        }
        if (!rtp_socket || !rtcp_socket)
        {
            return rtsp_server_reply_setup(server_, 500, nullptr, nullptr);
        }
        state.rtp_socket = std::move(rtp_socket);
        state.rtcp_socket = std::move(rtcp_socket);
        state.client_endpoint = boost::asio::ip::udp::endpoint(client_address, transport->rtp.u.client_port1);
    }
    state.setup = true;
    state.rtp_channel = transport->transport == RTSP_TRANSPORT_RTP_TCP ? transport->interleaved1 : -1;
    state.rtcp_channel = transport->transport == RTSP_TRANSPORT_RTP_TCP ? transport->interleaved2 : -1;
    rtsp_server_set_session_timeout(server_, 60);
    const auto transport_name = transport->transport == RTSP_TRANSPORT_RTP_TCP
                                     ? "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(state.rtp_channel) + "-" +
                                           std::to_string(state.rtcp_channel) + ";mode=record"
                                     : "RTP/AVP;unicast;client_port=" + std::to_string(transport->rtp.u.client_port1) + "-" +
                                           std::to_string(transport->rtp.u.client_port2) + ";server_port=" +
                                           std::to_string(state.rtp_socket->local_port()) + "-" +
                                           std::to_string(state.rtcp_socket->local_port()) + ";mode=record";
    const std::string response = transport_name;
    return rtsp_server_reply_setup(server_, 200, session_id_.c_str(), response.c_str());
}

int rtsp_publish_session::on_record(std::string_view, std::string_view session)
{
    if (!announced_ || recording_ || session != session_id_)
    {
        return rtsp_server_reply_record(server_, 454, nullptr, nullptr);
    }
    for (std::size_t index = 0; index < media_count_; ++index)
    {
        if (!media_[index].setup || !media_[index].track)
        {
            return rtsp_server_reply_record(server_, 455, nullptr, nullptr);
        }
    }

    std::vector<media_track> tracks;
    tracks.reserve(media_count_);
    for (std::size_t index = 0; index < media_count_; ++index)
    {
        tracks.push_back(*media_[index].track);
    }
    std::ranges::sort(tracks, [](const media_track& left, const media_track& right) { return left.id < right.id; });
    if (!stream_->set_tracks(std::move(tracks)) || !registry_.add(stream_))
    {
        rtsp_server_reply_record(server_, 453, nullptr, nullptr);
        shutdown();
        return 0;
    }
    recording_ = true;
    return rtsp_server_reply_record(server_, 200, nullptr, nullptr);
}

int rtsp_publish_session::on_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || !recording_ || closed_ || !stream_)
    {
        return -1;
    }

    const auto codecid = packet->stream->codecid;
    const bool video = codecid == AVCODEC_VIDEO_H264 || codecid == AVCODEC_VIDEO_H265;
    const bool audio = codecid == AVCODEC_AUDIO_AAC || codecid == AVCODEC_AUDIO_OPUS || codecid == AVCODEC_AUDIO_G711A ||
                       codecid == AVCODEC_AUDIO_G711U;
    const auto id = video ? video_track_id : (audio ? audio_track_id : 0);
    if (id == 0)
    {
        return 0;
    }
    const auto codec = codecid == AVCODEC_VIDEO_H264       ? codec_id::h264
                       : codecid == AVCODEC_VIDEO_H265     ? codec_id::h265
                       : codecid == AVCODEC_AUDIO_AAC      ? codec_id::aac
                       : codecid == AVCODEC_AUDIO_OPUS     ? codec_id::opus
                       : codecid == AVCODEC_AUDIO_G711A    ? codec_id::g711a
                                                            : codec_id::g711u;
    const auto state = std::find_if(media_.begin(), media_.begin() + static_cast<std::ptrdiff_t>(media_count_), [codec](const media_state& value)
                                    { return value.track && value.track->codec == codec; });
    if (state == media_.begin() + static_cast<std::ptrdiff_t>(media_count_))
    {
        spdlog::warn("rtsp publish raw codec change {}", to_string(codec));
        shutdown();
        return -1;
    }

    if (update_track_from_packet(*packet))
    {
        avpkt2bs_destroy(&bitstream_);
        static_cast<void>(avpkt2bs_create(&bitstream_));
    }
    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    stream_->publish(media_frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    });
    return 0;
}

bool rtsp_publish_session::update_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    std::optional<media_track> track;
    if (input.codecid == AVCODEC_VIDEO_H264 && input.extra != nullptr && input.bytes > 0)
    {
        auto config = h264_avcc_to_annex_b(
            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        if (!config.empty())
        {
            track = media_track{.id = video_track_id, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (input.codecid == AVCODEC_VIDEO_H265 && input.extra != nullptr && input.bytes > 0)
    {
        auto config = h265_hvcc_to_annex_b(
            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        if (!config.empty())
        {
            track = media_track{.id = video_track_id, .kind = media_kind::video, .codec = codec_id::h265, .clock_rate = 90'000, .codec_config = std::move(config)};
        }
    }
    else if (input.codecid == AVCODEC_AUDIO_AAC && input.extra != nullptr && input.bytes > 0 && input.sample_rate > 0 && input.channels > 0)
    {
        const auto* begin = static_cast<const std::uint8_t*>(input.extra);
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = codec_id::aac,
                            .clock_rate = static_cast<std::uint32_t>(input.sample_rate),
                            .channel_count = static_cast<std::uint16_t>(input.channels),
                            .codec_config = std::vector<std::uint8_t>(begin, begin + input.bytes)};
    }
    else if ((input.codecid == AVCODEC_AUDIO_G711A || input.codecid == AVCODEC_AUDIO_G711U) && input.sample_rate == 8'000 && input.channels == 1)
    {
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = input.codecid == AVCODEC_AUDIO_G711A ? codec_id::g711a : codec_id::g711u,
                            .clock_rate = 8'000,
                            .channel_count = 1,
                            .codec_config = {}};
    }
    else if (input.codecid == AVCODEC_AUDIO_OPUS && input.sample_rate == 48'000 && (input.channels == 1 || input.channels == 2))
    {
        track = media_track{.id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = codec_id::opus,
                            .clock_rate = 48'000,
                            .channel_count = static_cast<std::uint16_t>(input.channels),
                            .codec_config = {}};
    }
    if (!track)
    {
        return false;
    }

    const auto state = std::find_if(media_.begin(), media_.begin() + static_cast<std::ptrdiff_t>(media_count_), [track](const media_state& value)
                                    { return value.track && value.track->codec == track->codec; });
    if (state == media_.begin() + static_cast<std::ptrdiff_t>(media_count_))
    {
        return false;
    }
    if (!stream_->update_track(*track))
    {
        return false;
    }
    state->track = std::move(track);
    return true;
}

void rtsp_publish_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_)
    {
        registry_.remove(*stream_);
        stream_->end();
    }
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
        server_ = nullptr;
    }
    if (interleaved_.data != nullptr)
    {
        std::free(interleaved_.data);
        interleaved_.data = nullptr;
    }
    interleaved_.capacity = 0;
    interleaved_.bytes = 0;
    interleaved_.length = 0;
    interleaved_.state = 0;
    reset_media();
    avpkt2bs_destroy(&bitstream_);
    spdlog::debug("rtsp publish shutdown {}", stream_name_);
}

void rtsp_publish_session::reset_media()
{
    for (auto& state : media_)
    {
        if (state.rtp_socket)
        {
            state.rtp_socket->shutdown();
        }
        if (state.rtcp_socket)
        {
            state.rtcp_socket->shutdown();
        }
        if (state.demuxer != nullptr)
        {
            rtsp_demuxer_destroy(state.demuxer);
        }
        state = {};
    }
    media_descriptions_.clear();
    media_count_ = 0;
    stream_.reset();
}

}    // namespace media_server
