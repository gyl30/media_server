#include "media/rtsp/rtsp_input_session.h"

#include "media/codec/codec_utils.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "avpacket.h"
#include "base64.h"
#include "avstream.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "sdp-a-fmtp.h"
#include "sdp.h"
}

#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

bool append_sdp_parameter_sets(std::vector<std::uint8_t>& config, std::string_view encoded)
{
    constexpr std::array<std::uint8_t, 4> start_code{0x00, 0x00, 0x00, 0x01};
    while (!encoded.empty())
    {
        const auto comma = encoded.find(',');
        const auto parameter_set = encoded.substr(0, comma);
        if (parameter_set.empty())
        {
            return false;
        }

        std::vector<std::uint8_t> decoded((parameter_set.size() + 3U) / 4U * 3U);
        const auto bytes = base64_decode(decoded.data(), parameter_set.data(), parameter_set.size());
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

std::optional<std::uint16_t> opus_channel_count_from_fmtp(const char* fmtp)
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
                                                 [](unsigned char left, unsigned char right)
                                                 { return std::tolower(left) == std::tolower(right); });
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

bool should_setup_media(rtsp_client_t* client, int media)
{
    const auto type = rtsp_client_get_media_type(client, media);
    const auto* encoding = rtsp_client_get_media_encoding(client, media);
    const bool supported = (type == SDP_M_MEDIA_VIDEO && (iequals(encoding, "H264") || iequals(encoding, "H265") || iequals(encoding, "HEVC"))) ||
                           (type == SDP_M_MEDIA_AUDIO &&
                            (iequals(encoding, "MPEG4-GENERIC") ||
                             (iequals(encoding, "opus") && rtsp_client_get_media_rate(client, media) == 48'000 &&
                              opus_channel_count_from_fmtp(rtsp_client_get_media_fmtp(client, media)).has_value())));
    if (!supported)
    {
        return false;
    }

    // ireader 会压缩被忽略的 media，因此当前索引之前只有已经选择的 media。
    for (int selected = 0; selected < media; ++selected)
    {
        if (rtsp_client_get_media_type(client, selected) == type)
        {
            return false;
        }
    }
    return true;
}
}    // namespace

rtsp_input_session::rtsp_input_session(boost::asio::io_context& io,
                                       stream_registry& registry,
                                       std::string stream_name,
                                       std::string url,
                                       std::chrono::milliseconds establishment_timeout,
                                       std::chrono::milliseconds initial_tracks_timeout)
    : io_(io),
      registry_(registry),
      stream_name_(std::move(stream_name)),
      url_(std::move(url)),
      resolver_(io),
      connect_socket_(io),
      establishment_timer_(io),
      initial_tracks_timer_(io),
      establishment_timeout_(establishment_timeout),
      initial_tracks_timeout_(initial_tracks_timeout)
{
}

rtsp_input_session::~rtsp_input_session()
{
    for (auto*& demuxer : demuxers_)
    {
        if (demuxer != nullptr)
        {
            rtsp_demuxer_destroy(demuxer);
        }
    }
    if (client_ != nullptr)
    {
        rtsp_client_destroy(client_);
    }
    avpkt2bs_destroy(&bitstream_);
}

bool rtsp_input_session::startup()
{
    if (closed_ || stream_)
    {
        return false;
    }
    const auto parsed = parse_url(url_);
    if (!parsed)
    {
        return false;
    }

    url_ = parsed->request_url;
    username_ = parsed->username;
    password_ = parsed->password;

    stream_ = std::make_shared<media_stream>(stream_name_, io_.get_executor());

    static_cast<void>(avpkt2bs_create(&bitstream_));

    record_establishment_progress();
    wait_establishment_timeout();

    const auto self = shared_from_this();
    resolver_.async_resolve(parsed->host,
                            std::to_string(parsed->port),
                            [this, self](const boost::system::error_code& error, boost::asio::ip::tcp::resolver::results_type endpoints)
                            {
                                if (closed_)
                                {
                                    return;
                                }
                                if (error)
                                {
                                    shutdown();
                                    return;
                                }
                                record_establishment_progress();
                                boost::asio::async_connect(connect_socket_,
                                                           endpoints,
                                                           [this, self](const boost::system::error_code& connect_error,
                                                                        const boost::asio::ip::tcp::endpoint&)
                                                           { on_connect(connect_error, std::move(connect_socket_)); });
                            });
    return true;
}

void rtsp_input_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(io_, [self]() { self->safe_shutdown(); });
}

void rtsp_input_session::record_establishment_progress()
{
    last_establishment_progress_ = std::chrono::steady_clock::now();
}

void rtsp_input_session::wait_establishment_timeout()
{
    establishment_timer_.expires_at(last_establishment_progress_ + establishment_timeout_);
    const auto self = shared_from_this();
    establishment_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || self->media_started_)
            {
                return;
            }

            const auto deadline = self->last_establishment_progress_ + self->establishment_timeout_;
            if (std::chrono::steady_clock::now() < deadline)
            {
                self->wait_establishment_timeout();
                return;
            }

            spdlog::warn("rtsp input establishment timeout stream {}", self->stream_name_);
            self->shutdown();
        });
}

void rtsp_input_session::safe_shutdown()
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
        stream_.reset();
    }
    keepalive_deadline_.reset();
    establishment_timer_.cancel();
    initial_tracks_timer_.cancel();
    resolver_.cancel();
    boost::system::error_code error;
    connect_socket_.close(error);
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
    for (auto*& demuxer : demuxers_)
    {
        if (demuxer != nullptr)
        {
            rtsp_demuxer_destroy(demuxer);
            demuxer = nullptr;
        }
    }
    if (client_ != nullptr)
    {
        rtsp_client_destroy(client_);
        client_ = nullptr;
    }
    avpkt2bs_destroy(&bitstream_);
    spdlog::debug("rtsp input shutdown {}", stream_name_);
}

int rtsp_input_session::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_input_session*>(param);
    if (!self->connection_)
    {
        return -1;
    }
    if (!self->media_started_)
    {
        self->record_establishment_progress();
    }
    self->connection_->write(request, bytes);
    return static_cast<int>(bytes);
}

int rtsp_input_session::rtp_port_callback(void* param, int media, const char*, unsigned short port[2], char*, int)
{
    auto* self = static_cast<rtsp_input_session*>(param);
    if (self->client_ == nullptr)
    {
        return -1;
    }
    if (!should_setup_media(self->client_, media))
    {
        const auto* encoding = rtsp_client_get_media_encoding(self->client_, media);
        spdlog::debug("rtsp input ignore media {} encoding {}", media, encoding != nullptr ? encoding : "");
        return 0;
    }
    port[0] = static_cast<unsigned short>(media * 2);
    port[1] = static_cast<unsigned short>(media * 2 + 1);
    return RTSP_TRANSPORT_RTP_TCP;
}

int rtsp_input_session::describe_callback(void* param, const char* sdp, int length)
{
    return static_cast<rtsp_input_session*>(param)->on_describe(sdp, length);
}

int rtsp_input_session::setup_callback(void* param, int timeout, std::int64_t duration)
{
    return static_cast<rtsp_input_session*>(param)->on_setup(timeout, duration);
}

int rtsp_input_session::play_callback(void*, int, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t*, int)
{
    return 0;
}

int rtsp_input_session::pause_callback(void*) { return 0; }

int rtsp_input_session::teardown_callback(void*) { return 0; }

void rtsp_input_session::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    static_cast<rtsp_input_session*>(param)->on_rtp(channel, data, bytes);
}

int rtsp_input_session::packet_callback(void* param, avpacket_t* packet) { return static_cast<rtsp_input_session*>(param)->on_packet(packet); }

std::optional<rtsp_input_session::parsed_url> rtsp_input_session::parse_url(std::string_view url)
{
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed || parsed->scheme() != "rtsp" || !parsed->has_authority())
    {
        return std::nullopt;
    }

    const auto host = parsed->host_address();
    if (host.empty())
    {
        return std::nullopt;
    }

    if (parsed->has_port() && parsed->port_number() == 0)
    {
        return std::nullopt;
    }

    boost::urls::url request_url(*parsed);
    request_url.remove_userinfo();

    parsed_url result;
    result.request_url = std::string(request_url.buffer());
    result.host = std::string(host);
    result.port = parsed->has_port() ? parsed->port_number() : static_cast<std::uint16_t>(554);
    result.username = std::string(parsed->user());
    result.password = std::string(parsed->password());
    return result;
}

void rtsp_input_session::on_connect(const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
{
    if (closed_)
    {
        return;
    }
    if (error)
    {
        shutdown();
        return;
    }

    rtsp_client_handler_t handler{};
    handler.send = &rtsp_input_session::send_callback;
    handler.rtpport = &rtsp_input_session::rtp_port_callback;
    handler.ondescribe = &rtsp_input_session::describe_callback;
    handler.onsetup = &rtsp_input_session::setup_callback;
    handler.onplay = &rtsp_input_session::play_callback;
    handler.onpause = &rtsp_input_session::pause_callback;
    handler.onteardown = &rtsp_input_session::teardown_callback;
    handler.onrtp = &rtsp_input_session::rtp_callback;

    client_ = rtsp_client_create(
        url_.c_str(), username_.empty() ? nullptr : username_.c_str(), password_.empty() ? nullptr : password_.c_str(), &handler, this);
    if (client_ == nullptr)
    {
        shutdown();
        return;
    }

    connection_ = std::make_shared<tcp_connection>(std::move(socket));
    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code read_error, std::span<const std::uint8_t> data)
        {
            if (read_error)
            {
                self->shutdown();
                return;
            }
            self->on_read(data);
        },
        [self](boost::system::error_code write_error, std::size_t)
        {
            if (write_error)
            {
                self->shutdown();
            }
        });

    spdlog::info("rtsp input connected stream {}", stream_name_);
    if (rtsp_client_describe(client_) != 0)
    {
        shutdown();
    }
}

void rtsp_input_session::on_read(std::span<const std::uint8_t> data)
{
    if (client_ != nullptr && rtsp_client_input(client_, data.data(), data.size()) != 0)
    {
        shutdown();
    }
}

int rtsp_input_session::on_describe(const char* sdp, int length)
{
    spdlog::debug("rtsp input describe {}", stream_name_);
    return rtsp_client_setup(client_, sdp, length);
}

int rtsp_input_session::on_setup(int timeout, std::int64_t)
{
    const auto keepalive_seconds = timeout > 0 ? std::max(timeout / 2, 1) : 30;
    keepalive_interval_ = std::chrono::seconds(keepalive_seconds);

    const auto media_count = rtsp_client_media_count(client_);
    if (media_count < 0 || static_cast<std::size_t>(media_count) > demuxers_.size())
    {
        return -1;
    }

    bool expected_video = false;
    for (int media = 0; media < media_count; ++media)
    {
        const auto media_type = rtsp_client_get_media_type(client_, media);
        expected_video = expected_video || media_type == SDP_M_MEDIA_VIDEO;
        expected_audio_ = expected_audio_ || media_type == SDP_M_MEDIA_AUDIO;

        const auto* encoding = rtsp_client_get_media_encoding(client_, media);
        const auto* fmtp = rtsp_client_get_media_fmtp(client_, media);
        const auto rate = rtsp_client_get_media_rate(client_, media);
        const auto payload = rtsp_client_get_media_payload(client_, media);
        auto*& demuxer = demuxers_[static_cast<std::size_t>(media)];
        demuxer = rtsp_demuxer_create(media, 500, &rtsp_input_session::packet_callback, this);
        if (demuxer == nullptr || rtsp_demuxer_add_payload(demuxer, rate, payload, encoding, fmtp) != 0)
        {
            return -1;
        }

        std::optional<media_track> track;
        if (rtsp_client_get_media_type(client_, media) == SDP_M_MEDIA_VIDEO && iequals(encoding, "H264"))
        {
            sdp_a_fmtp_h264_t parameters{};
            auto format = payload;
            std::vector<std::uint8_t> config;
            if (fmtp != nullptr && sdp_a_fmtp_h264(fmtp, &format, &parameters) == 0 &&
                append_sdp_parameter_sets(config, parameters.sprop_parameter_sets) && !h264_annex_b_to_avcc(config).empty())
            {
                track = media_track{
                    .id = video_track_id,
                    .kind = media_kind::video,
                    .codec = codec_id::h264,
                    .clock_rate = 90'000,
                    .channel_count = 0,
                    .codec_config = std::move(config),
                };
            }
        }
        else if (rtsp_client_get_media_type(client_, media) == SDP_M_MEDIA_VIDEO &&
                 (iequals(encoding, "H265") || iequals(encoding, "HEVC")))
        {
            sdp_a_fmtp_h265_t parameters{};
            auto format = payload;
            std::vector<std::uint8_t> config;
            if (fmtp != nullptr && sdp_a_fmtp_h265(fmtp, &format, &parameters) == 0 &&
                append_sdp_parameter_sets(config, parameters.sprop_vps) && append_sdp_parameter_sets(config, parameters.sprop_sps) &&
                append_sdp_parameter_sets(config, parameters.sprop_pps) && !h265_annex_b_to_hvcc(config).empty())
            {
                track = media_track{
                    .id = video_track_id,
                    .kind = media_kind::video,
                    .codec = codec_id::h265,
                    .clock_rate = 90'000,
                    .channel_count = 0,
                    .codec_config = std::move(config),
                };
            }
        }
        else if (rtsp_client_get_media_type(client_, media) == SDP_M_MEDIA_AUDIO && iequals(encoding, "MPEG4-GENERIC"))
        {
            sdp_a_fmtp_mpeg4_t parameters{};
            auto format = payload;
            if (fmtp != nullptr && sdp_a_fmtp_mpeg4(fmtp, &format, &parameters) == 0)
            {
                const std::string_view encoded(parameters.config);
                if (!encoded.empty() && (encoded.size() % 2U) == 0U &&
                    std::all_of(encoded.begin(), encoded.end(), [](char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; }))
                {
                    std::vector<std::uint8_t> config(encoded.size() / 2U);
                    static_cast<void>(base16_decode(config.data(), encoded.data(), encoded.size()));
                    if (const auto aac = parse_aac_asc(config))
                    {
                        track = media_track{
                            .id = audio_track_id,
                            .kind = media_kind::audio,
                            .codec = codec_id::aac,
                            .clock_rate = aac->sample_rate,
                            .channel_count = aac->channel_count,
                            .codec_config = std::move(config),
                        };
                    }
                }
            }
        }
        else if (rtsp_client_get_media_type(client_, media) == SDP_M_MEDIA_AUDIO && iequals(encoding, "opus") && rate == 48'000)
        {
            if (const auto channel_count = opus_channel_count_from_fmtp(fmtp))
            {
                track = media_track{
                    .id = audio_track_id,
                    .kind = media_kind::audio,
                    .codec = codec_id::opus,
                    .clock_rate = 48'000,
                    .channel_count = *channel_count,
                    .codec_config = {},
                };
            }
        }

        if (track)
        {
            auto& pending = track->kind == media_kind::video ? initial_video_track_ : initial_audio_track_;
            pending = std::move(*track);
        }
    }
    if (!expected_video)
    {
        return -1;
    }

    keepalive_deadline_ = std::chrono::steady_clock::now() + keepalive_interval_;
    std::uint64_t npt{};
    return rtsp_client_play(client_, &npt, nullptr);
}

void rtsp_input_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    const auto media = static_cast<std::size_t>(channel / 2U);
    if (media >= demuxers_.size() || demuxers_[media] == nullptr || (channel % 2U) != 0U)
    {
        return;
    }

    if (!media_started_)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= last_establishment_progress_ + establishment_timeout_)
        {
            spdlog::warn("rtsp input establishment timeout stream {}", stream_name_);
            shutdown();
            return;
        }
        media_started_ = true;
        establishment_timer_.cancel();
        static_cast<void>(try_initialize_tracks());
        if (!tracks_initialized_)
        {
            initial_tracks_timer_.expires_after(initial_tracks_timeout_);
            const auto self = shared_from_this();
            initial_tracks_timer_.async_wait(
                [self](const boost::system::error_code& error)
                {
                    if (error || self->closed_ || self->tracks_initialized_)
                    {
                        return;
                    }
                    spdlog::warn("rtsp input initial tracks timeout stream {}", self->stream_name_);
                    self->shutdown();
                });
        }
    }

    static_cast<void>(rtsp_demuxer_input(demuxers_[media], data, static_cast<int>(bytes)));

    if (closed_ || client_ == nullptr || !keepalive_deadline_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < *keepalive_deadline_)
    {
        return;
    }

    keepalive_deadline_ = now + keepalive_interval_;
    if (rtsp_client_options(client_, nullptr) != 0)
    {
        shutdown();
    }
}

int rtsp_input_session::on_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || closed_ || !stream_)
    {
        return -1;
    }

    // ireader 会随码流更新 packet.stream 中的配置，核心负责过滤未变化配置。
    // avpkt2bs 会缓存首次解析的编解码配置，配置代际变化时重置后再转换当前帧。
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

    track_id id{};
    if (packet->stream->codecid == AVCODEC_VIDEO_H264 || packet->stream->codecid == AVCODEC_VIDEO_H265)
    {
        id = video_track_id;
    }
    else if (packet->stream->codecid == AVCODEC_AUDIO_AAC || packet->stream->codecid == AVCODEC_AUDIO_OPUS)
    {
        id = audio_track_id;
    }
    else
    {
        return 0;
    }

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(bitstream_.ptr, bitstream_.ptr + bytes);
    media_frame frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    };
    stream_->publish(std::move(frame));
    return 0;
}

bool rtsp_input_session::update_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    std::optional<media_track> track;

    if (input.codecid == AVCODEC_VIDEO_H264)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            config = h264_avcc_to_annex_b(
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        }
        if (!config.empty())
        {
            track = media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = std::move(config),
            };
        }
    }
    else if (input.codecid == AVCODEC_VIDEO_H265)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            config = h265_hvcc_to_annex_b(
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        }
        if (!config.empty())
        {
            track = media_track{
                .id = video_track_id,
                .kind = media_kind::video,
                .codec = codec_id::h265,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = std::move(config),
            };
        }
    }
    else if (input.codecid == AVCODEC_AUDIO_AAC)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            const auto* begin = static_cast<const std::uint8_t*>(input.extra);
            config.assign(begin, begin + input.bytes);
        }
        if (!config.empty() && input.sample_rate > 0 && input.channels > 0)
        {
            track = media_track{
                .id = audio_track_id,
                .kind = media_kind::audio,
                .codec = codec_id::aac,
                .clock_rate = static_cast<std::uint32_t>(input.sample_rate),
                .channel_count = static_cast<std::uint16_t>(input.channels),
                .codec_config = std::move(config),
            };
        }
    }

    if (!track)
    {
        return false;
    }

    if (tracks_initialized_)
    {
        const bool changed = stream_->update_track(*track);
        if (changed)
        {
            spdlog::info("rtsp input track {} {}", to_string(track->kind), to_string(track->codec));
        }
        return changed;
    }

    bool changed = false;
    auto& pending = track->kind == media_kind::video ? initial_video_track_ : initial_audio_track_;
    if (pending)
    {
        changed = pending->codec != track->codec || pending->clock_rate != track->clock_rate || pending->channel_count != track->channel_count ||
                  pending->codec_config != track->codec_config;
    }
    pending = *track;

    if (std::chrono::steady_clock::now() >= initial_tracks_timer_.expiry())
    {
        shutdown();
        return changed;
    }
    return try_initialize_tracks() || changed;
}

bool rtsp_input_session::try_initialize_tracks()
{
    if (tracks_initialized_ || !initial_video_track_ || (expected_audio_ && !initial_audio_track_))
    {
        return false;
    }

    std::vector<media_track> tracks;
    tracks.push_back(std::move(*initial_video_track_));
    if (expected_audio_)
    {
        tracks.push_back(std::move(*initial_audio_track_));
    }
    tracks_initialized_ = stream_->set_tracks(std::move(tracks));
    initial_video_track_.reset();
    initial_audio_track_.reset();
    if (!tracks_initialized_)
    {
        return false;
    }
    if (!registry_.add(stream_))
    {
        spdlog::warn("rtsp input duplicate stream {}", stream_name_);
        shutdown();
        return true;
    }
    initial_tracks_timer_.cancel();
    spdlog::info("rtsp input tracks ready audio {}", expected_audio_);
    return true;
}

}    // namespace media_server
