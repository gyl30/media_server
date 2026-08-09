#include "media/rtsp/rtsp_input_session.h"

#include "media/codec/codec_utils.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "avpacket.h"
#include "avstream.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "sdp.h"
}

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

bool should_setup_media(rtsp_client_t* client, int media)
{
    const auto type = rtsp_client_get_media_type(client, media);
    const auto* encoding = rtsp_client_get_media_encoding(client, media);
    const bool supported = (type == SDP_M_MEDIA_VIDEO && (iequals(encoding, "H264") || iequals(encoding, "H265") || iequals(encoding, "HEVC"))) ||
                           (type == SDP_M_MEDIA_AUDIO && iequals(encoding, "MPEG4-GENERIC"));
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

rtsp_input_session::rtsp_input_session(boost::asio::io_context& io, stream_registry& registry, std::string stream_name, std::string url)
    : io_(io), registry_(registry), stream_name_(std::move(stream_name)), url_(std::move(url)), resolver_(io)
{
}

rtsp_input_session::~rtsp_input_session()
{
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
    }
    avpkt2bs_destroy(&bitstream_);
}

bool rtsp_input_session::start()
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

    stream_ = std::make_shared<media_stream>(stream_name_);
    if (!registry_.add(stream_))
    {
        stream_.reset();
        return false;
    }

    static_cast<void>(avpkt2bs_create(&bitstream_));

    const auto self = shared_from_this();
    resolver_.async_resolve(parsed->host,
                            std::to_string(parsed->port),
                            [this, self](const boost::system::error_code& error, boost::asio::ip::tcp::resolver::results_type endpoints)
                            {
                                if (error)
                                {
                                    close();
                                    return;
                                }
                                auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
                                boost::asio::async_connect(*socket,
                                                           endpoints,
                                                           [this, self, socket](const boost::system::error_code& connect_error,
                                                                                const boost::asio::ip::tcp::endpoint&) mutable
                                                           { on_connect(connect_error, std::move(*socket)); });
                            });
    return true;
}

void rtsp_input_session::close()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    keepalive_deadline_.reset();
    resolver_.cancel();
    if (connection_)
    {
        connection_->close();
    }
    if (stream_)
    {
        stream_->end();
        registry_.remove(*stream_);
        stream_.reset();
    }
    spdlog::debug("rtsp input close {}", stream_name_);
}

int rtsp_input_session::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_input_session*>(param);
    if (!self->connection_)
    {
        return -1;
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

int rtsp_input_session::play_callback(void* param, int, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t*, int)
{
    return static_cast<rtsp_input_session*>(param)->on_play();
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
    if (error || closed_)
    {
        close();
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
        close();
        return;
    }

    connection_ = std::make_shared<tcp_connection>(std::move(socket));
    const auto self = shared_from_this();
    connection_->start([self](std::span<const std::uint8_t> data) { self->on_read(data); }, [self]() { self->on_connection_close(); });

    spdlog::info("rtsp input connected stream {}", stream_name_);
    if (rtsp_client_describe(client_) != 0)
    {
        close();
    }
}

void rtsp_input_session::on_read(std::span<const std::uint8_t> data)
{
    if (client_ != nullptr && rtsp_client_input(client_, data.data(), data.size()) != 0)
    {
        close();
    }
}

void rtsp_input_session::on_connection_close()
{
    if (!closed_)
    {
        close();
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

    for (int media = 0; media < media_count; ++media)

    {
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
    }

    std::uint64_t npt{};
    return rtsp_client_play(client_, &npt, nullptr);
}

int rtsp_input_session::on_play()
{
    keepalive_deadline_ = std::chrono::steady_clock::now() + keepalive_interval_;
    return 0;
}

void rtsp_input_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    const auto media = static_cast<std::size_t>(channel / 2U);
    if (media >= demuxers_.size() || demuxers_[media] == nullptr || (channel % 2U) != 0U)
    {
        return;
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
        close();
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
    else if (packet->stream->codecid == AVCODEC_AUDIO_AAC)
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
    if (input.codecid == AVCODEC_VIDEO_H264)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            config = h264_avcc_to_annex_b(
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        }
        if (config.empty())
        {
            return false;
        }
        media_track track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = codec_id::h264,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = std::move(config),
        };
        const bool changed = stream_->update_track(std::move(track));
        if (changed)
        {
            spdlog::info("rtsp input track video h264");
        }
        return changed;
    }

    if (input.codecid == AVCODEC_VIDEO_H265)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            config = h265_hvcc_to_annex_b(
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        }
        if (config.empty())
        {
            return false;
        }
        media_track track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = codec_id::h265,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = std::move(config),
        };
        const bool changed = stream_->update_track(std::move(track));
        if (changed)
        {
            spdlog::info("rtsp input track video h265");
        }
        return changed;
    }

    if (input.codecid == AVCODEC_AUDIO_AAC)

    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            const auto* begin = static_cast<const std::uint8_t*>(input.extra);
            config.assign(begin, begin + input.bytes);
        }
        if (config.empty() || input.sample_rate <= 0 || input.channels <= 0)
        {
            return false;
        }
        media_track track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = codec_id::aac,
            .clock_rate = static_cast<std::uint32_t>(input.sample_rate),
            .channel_count = static_cast<std::uint16_t>(input.channels),
            .codec_config = std::move(config),
        };
        const bool changed = stream_->update_track(std::move(track));
        if (changed)
        {
            spdlog::info("rtsp input track audio aac sample_rate {} channels {}", input.sample_rate, input.channels);
        }
        return changed;
    }

    return false;
}

}    // namespace media_server
