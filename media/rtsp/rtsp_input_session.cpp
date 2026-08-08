#include "media/rtsp/rtsp_input_session.h"

#include "media/codec/codec_utils.h"
#include "media/core/log.h"

extern "C"
{
#include "avpacket.h"
#include "avstream.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
#include "sdp.h"
}

#include <boost/url/parse.hpp>

#include <cstring>
#include <optional>
#include <utility>

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
}

rtsp_input_session::rtsp_input_session(
    boost::asio::io_context& io,
    stream_registry& registry,
    std::string stream_name,
    std::string url,
    std::string username,
    std::string password)
    : io_(io),
      registry_(registry),
      stream_name_(std::move(stream_name)),
      url_(std::move(url)),
      username_(std::move(username)),
      password_(std::move(password)),
      resolver_(io)
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
    if (bitstream_created_)
    {
        avpkt2bs_destroy(&bitstream_);
    }
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

    stream_ = std::make_shared<media_stream>(stream_name_);
    if (!registry_.add(stream_))
    {
        stream_.reset();
        return false;
    }

    if (avpkt2bs_create(&bitstream_) != 0)

    {
        registry_.remove(stream_name_, stream_.get());
        stream_.reset();
        return false;
    }
    bitstream_created_ = true;

    const auto self = shared_from_this();
    resolver_.async_resolve(
        parsed->host,
        std::to_string(parsed->port),
        [this, self](const boost::system::error_code& error, boost::asio::ip::tcp::resolver::results_type endpoints) {
            if (error)
            {
                close();
                return;
            }
            auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
            boost::asio::async_connect(
                *socket,
                endpoints,
                [this, self, socket](const boost::system::error_code& connect_error, const boost::asio::ip::tcp::endpoint&) mutable {
                    on_connect(connect_error, std::move(*socket));
                });
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
    resolver_.cancel();
    if (connection_)
    {
        connection_->close();
    }
    if (stream_)
    {
        stream_->end();
        registry_.remove(stream_name_, stream_.get());
        stream_.reset();
    }
    log_line("rtsp_input", "close", stream_name_);
}

int rtsp_input_session::send_callback(
    void* param,
    const char*,
    const void* request,
    std::size_t bytes)
    {
    auto* self = static_cast<rtsp_input_session*>(param);
    if (!self->connection_)
    {
        return -1;
    }
    self->connection_->write(request, bytes);
    return static_cast<int>(bytes);
}

int rtsp_input_session::rtp_port_callback(
    void* param,
    int media,
    const char*,
    unsigned short port[2],
    char*,
    int)
    {
    auto* self = static_cast<rtsp_input_session*>(param);
    if (self->client_ == nullptr)
    {
        return -1;
    }
    const auto type = rtsp_client_get_media_type(self->client_, media);
    if (type != SDP_M_MEDIA_AUDIO && type != SDP_M_MEDIA_VIDEO)
    {
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

int rtsp_input_session::play_callback(
    void*, int, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t*, int)
    {
    return 0;
}

int rtsp_input_session::pause_callback(void*)
{
    return 0;
}

int rtsp_input_session::teardown_callback(void*)
{
    return 0;
}

void rtsp_input_session::rtp_callback(
    void* param,
    std::uint8_t channel,
    const void* data,
    std::uint16_t bytes)
    {
    static_cast<rtsp_input_session*>(param)->on_rtp(channel, data, bytes);
}

int rtsp_input_session::packet_callback(void* param, avpacket_t* packet)
{
    return static_cast<rtsp_input_session*>(param)->on_packet(packet);
}

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

    parsed_url result;
    result.host = std::string(host);
    result.port = parsed->has_port() ? parsed->port_number() : static_cast<std::uint16_t>(554);
    return result;
}

void rtsp_input_session::on_connect(
    const boost::system::error_code& error,
    boost::asio::ip::tcp::socket socket)
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
        url_.c_str(),
        username_.empty() ? nullptr : username_.c_str(),
        password_.empty() ? nullptr : password_.c_str(),
        &handler,
        this);
    if (client_ == nullptr)
    {
        close();
        return;
    }

    connection_ = std::make_shared<tcp_connection>(std::move(socket));
    const auto self = shared_from_this();
    connection_->start(
        [self](std::span<const std::uint8_t> data) { self->on_read(data); },
        [self]() { self->on_connection_close(); });

    log_line("rtsp_input", "connected", url_, "stream", stream_name_);
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
    log_line("rtsp_input", "describe", stream_name_);
    return rtsp_client_setup(client_, sdp, length);
}

int rtsp_input_session::on_setup(int, std::int64_t)
{
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
        if (demuxer == nullptr ||
            rtsp_demuxer_add_payload(demuxer, rate, payload, encoding, fmtp) != 0)
            {
            return -1;
        }
    }

    std::uint64_t npt{};
    return rtsp_client_play(client_, &npt, nullptr);
}

void rtsp_input_session::on_rtp(
    std::uint8_t channel,
    const void* data,
    std::uint16_t bytes)
    {
    const auto media = static_cast<std::size_t>(channel / 2U);
    if (media >= demuxers_.size() || demuxers_[media] == nullptr || (channel % 2U) != 0U)
    {
        return;
    }
    static_cast<void>(rtsp_demuxer_input(demuxers_[media], data, static_cast<int>(bytes)));
}

int rtsp_input_session::on_packet(avpacket_t* packet)
{
    if (packet == nullptr || packet->stream == nullptr || closed_ || !stream_)
    {
        return -1;
    }

    publish_track_if_needed(*packet);
    const auto bytes = avpkt2bs_input(&bitstream_, packet);
    if (bytes <= 0 || bitstream_.ptr == nullptr)
    {
        return bytes < 0 ? bytes : 0;
    }

    track_id id{};
    std::int64_t duration_ns{};
    if (packet->stream->codecid == AVCODEC_VIDEO_H264)
    {
        id = video_track_id;
        if (packet->stream->fps > 0.0)
        {
            duration_ns = static_cast<std::int64_t>(1'000'000'000.0 / packet->stream->fps);
        }
    } else if (packet->stream->codecid == AVCODEC_AUDIO_AAC)
    {
        id = audio_track_id;
        if (packet->stream->sample_rate > 0)
        {
            duration_ns = 1'024'000'000'000LL / packet->stream->sample_rate;
        }
    }
    else
    {
        return 0;
    }

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(
        bitstream_.ptr, bitstream_.ptr + bytes);
    media_frame frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(packet->dts),
        .pts_ns = milliseconds_to_ns(packet->pts),
        .duration_ns = duration_ns,
        .key_frame = (packet->flags & AVPACKET_FLAG_KEY) != 0,
        .payload = std::move(payload),
    };
    stream_->publish(std::move(frame));
    return 0;
}

void rtsp_input_session::publish_track_if_needed(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    if (input.codecid == AVCODEC_VIDEO_H264 && !video_track_published_)
    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            config = h264_avcc_to_annex_b(std::span<const std::uint8_t>(
                static_cast<const std::uint8_t*>(input.extra), static_cast<std::size_t>(input.bytes)));
        }
        if (config.empty())
        {
            return;
        }
        media_track track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = codec_id::h264,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = std::move(config),
            .config_version = 1,
        };
        if (stream_->update_track(std::move(track)))
        {
            video_track_published_ = true;
            log_line("rtsp_input", "track video h264");
        }
    }

    if (input.codecid == AVCODEC_AUDIO_AAC && !audio_track_published_)

    {
        std::vector<std::uint8_t> config;
        if (input.extra != nullptr && input.bytes > 0)
        {
            const auto* begin = static_cast<const std::uint8_t*>(input.extra);
            config.assign(begin, begin + input.bytes);
        }
        if (config.empty() || input.sample_rate <= 0 || input.channels <= 0)
        {
            return;
        }
        media_track track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = codec_id::aac,
            .clock_rate = static_cast<std::uint32_t>(input.sample_rate),
            .channel_count = static_cast<std::uint16_t>(input.channels),
            .codec_config = std::move(config),
            .config_version = 1,
        };
        if (stream_->update_track(std::move(track)))
        {
            audio_track_published_ = true;
            log_line("rtsp_input", "track audio aac", input.sample_rate, input.channels);
        }
    }
}

}    // namespace media_server
