#include <chrono>
#include <utility>
#include <optional>
#include <algorithm>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>
#include <boost/url/url.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>
#include <boost/url/parse.hpp>

#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_sdp.h"
#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_pull_session.h"

extern "C"
{
#include "sdp.h"
#include "avpacket.h"
#include "avstream.h"
#include "rtp-profile.h"
#include "rtsp-client.h"
#include "rtsp-demuxer.h"
}

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
constexpr char rtcp_name[] = "media_server";
constexpr auto slow_write_timeout = std::chrono::seconds(15);

std::optional<codec_id> selected_g711_codec(rtsp_client_t* client, int media)
{
    if (rtsp_client_get_media_type(client, media) != SDP_M_MEDIA_AUDIO || rtsp_client_get_media_rate(client, media) != 8'000)
    {
        return std::nullopt;
    }

    const auto payload = rtsp_client_get_media_payload(client, media);
    const auto* encoding = rtsp_client_get_media_encoding(client, media);
    if (payload == RTP_PAYLOAD_PCMA && (encoding == nullptr || *encoding == '\0' || rtsp_sdp_iequals(encoding, "PCMA")))
    {
        return codec_id::g711a;
    }
    if (payload == RTP_PAYLOAD_PCMU && (encoding == nullptr || *encoding == '\0' || rtsp_sdp_iequals(encoding, "PCMU")))
    {
        return codec_id::g711u;
    }
    return std::nullopt;
}

bool should_setup_media(rtsp_client_t* client, int media)
{
    const auto type = rtsp_client_get_media_type(client, media);
    const auto* encoding = rtsp_client_get_media_encoding(client, media);
    const bool supported =
        (type == SDP_M_MEDIA_VIDEO &&
         (rtsp_sdp_iequals(encoding, "H264") || rtsp_sdp_iequals(encoding, "H265") || rtsp_sdp_iequals(encoding, "HEVC"))) ||
        (type == SDP_M_MEDIA_AUDIO && (rtsp_sdp_iequals(encoding, "MPEG4-GENERIC") ||
                                       (rtsp_sdp_iequals(encoding, "opus") && rtsp_client_get_media_rate(client, media) == 48'000 &&
                                        rtsp_sdp_opus_channel_count(rtsp_client_get_media_fmtp(client, media)).has_value()) ||
                                       selected_g711_codec(client, media).has_value()));
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

rtsp_pull_session::rtsp_pull_session(worker_context& worker,
                                     std::string stream_name,
                                     std::string url,
                                     std::chrono::milliseconds establishment_timeout,
                                     std::chrono::milliseconds initial_tracks_timeout)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      url_(std::move(url)),
      resolver_(worker_.io()),
      connect_socket_(worker_.io()),
      startup_timer_(worker_.io()),
      keepalive_timer_(worker_.io()),
      rtcp_timer_(worker_.io()),
      establishment_timeout_(establishment_timeout),
      initial_tracks_timeout_(initial_tracks_timeout)
{
}

rtsp_pull_session::~rtsp_pull_session() = default;

bool rtsp_pull_session::startup()
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

    stream_ = std::make_shared<media_stream>(stream_name_, worker_.io());

    static_cast<void>(avpkt2bs_create(&bitstream_));

    record_establishment_progress();
    schedule_establishment_timeout();

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(),
                       [self, host = parsed->host, port = parsed->port](boost::asio::yield_context yield)
                       { self->run(host, port, yield); },
                       boost::asio::detached);
    return true;
}

void rtsp_pull_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtsp_pull_session::record_establishment_progress() { last_establishment_progress_ = std::chrono::steady_clock::now(); }

void rtsp_pull_session::schedule_establishment_timeout()
{
    startup_timer_.expires_at(last_establishment_progress_ + establishment_timeout_);
    const auto self = shared_from_this();
    startup_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || self->media_started_)
            {
                return;
            }

            const auto deadline = self->last_establishment_progress_ + self->establishment_timeout_;
            if (std::chrono::steady_clock::now() < deadline)
            {
                self->schedule_establishment_timeout();
                return;
            }

            spdlog::warn("rtsp input establishment timeout stream {}", self->stream_name_);
            self->shutdown();
        });
}

void rtsp_pull_session::schedule_keepalive()
{
    keepalive_timer_.expires_after(keepalive_interval_);
    const auto self = shared_from_this();
    keepalive_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || self->client_ == nullptr)
            {
                return;
            }
            if (rtsp_client_options(self->client_, nullptr) != 0)
            {
                self->shutdown();
                return;
            }
            self->schedule_keepalive();
        });
}

void rtsp_pull_session::schedule_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_ || !self->media_started_ || !self->transport_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            for (std::size_t media = 0; media < self->demuxers_.size(); ++media)
            {
                if (self->demuxers_[media] == nullptr)
                {
                    continue;
                }
                const auto bytes = rtsp_demuxer_rtcp(self->demuxers_[media], buffer.data(), static_cast<int>(buffer.size()));
                if (bytes <= 0)
                {
                    continue;
                }

                std::vector<std::uint8_t> packet(static_cast<std::size_t>(bytes) + 4U);
                packet[0] = '$';
                packet[1] = static_cast<std::uint8_t>(media * 2U + 1U);
                packet[2] = static_cast<std::uint8_t>(bytes >> 8U);
                packet[3] = static_cast<std::uint8_t>(bytes);
                std::copy_n(buffer.begin(), bytes, packet.begin() + 4);
                self->write(packet);
            }
            self->schedule_rtcp();
        });
}

void rtsp_pull_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (stream_)
    {
        registry::instance().remove(*stream_);
        stream_->end();
        stream_.reset();
    }
    startup_timer_.cancel();
    keepalive_timer_.cancel();
    rtcp_timer_.cancel();
    resolver_.cancel();
    boost::system::error_code error;
    connect_socket_.close(error);
    if (transport_)
    {
        transport_->shutdown();
    }
    for (auto*& demuxer : demuxers_)
    {
        if (demuxer != nullptr)
        {
            rtsp_demuxer_destroy(demuxer);
            demuxer = nullptr;
        }
    }
    avpkt2bs_destroy(&bitstream_);
    spdlog::debug("rtsp input shutdown {}", stream_name_);
}

int rtsp_pull_session::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_pull_session*>(param);
    if (self->closed_ || !self->transport_)
    {
        return -1;
    }
    if (!self->media_started_)
    {
        self->record_establishment_progress();
    }
    self->write(std::span{static_cast<const std::uint8_t*>(request), bytes});
    return static_cast<int>(bytes);
}

int rtsp_pull_session::rtp_port_callback(void* param, int media, const char*, unsigned short port[2], char*, int)
{
    auto* self = static_cast<rtsp_pull_session*>(param);
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

int rtsp_pull_session::describe_callback(void* param, const char* sdp, int length)
{
    return static_cast<rtsp_pull_session*>(param)->on_describe(sdp, length);
}

int rtsp_pull_session::setup_callback(void* param, int timeout, std::int64_t duration)
{
    return static_cast<rtsp_pull_session*>(param)->on_setup(timeout, duration);
}

int rtsp_pull_session::play_callback(
    void* param, int media, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t* info, int count)
{
    if (info == nullptr || count == 0)
    {
        return 0;
    }

    auto* self = static_cast<rtsp_pull_session*>(param);
    return rtsp_demuxer_rtpinfo(
        self->demuxers_[static_cast<std::size_t>(media)], static_cast<std::uint16_t>(info[0].seq), info[0].time);
}

int rtsp_pull_session::pause_callback(void*) { return 0; }

int rtsp_pull_session::teardown_callback(void*) { return 0; }

void rtsp_pull_session::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    static_cast<rtsp_pull_session*>(param)->on_rtp(channel, data, bytes);
}

int rtsp_pull_session::packet_callback(void* param, avpacket_t* packet) { return static_cast<rtsp_pull_session*>(param)->on_demuxed_packet(packet); }

std::optional<rtsp_pull_session::parsed_url> rtsp_pull_session::parse_url(std::string_view url)
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

void rtsp_pull_session::run(std::string host, std::uint16_t port, boost::asio::yield_context yield)
{
    boost::system::error_code error;
    const auto endpoints = resolver_.async_resolve(host, std::to_string(port), yield[error]);
    if (error)
    {
        shutdown();
        return;
    }

    record_establishment_progress();
    boost::asio::async_connect(connect_socket_, endpoints, yield[error]);
    if (error)
    {
        shutdown();
        return;
    }

    transport_ = std::make_unique<tcp_yield_transport>(std::move(connect_socket_));

    rtsp_client_handler_t handler{};
    handler.send = &rtsp_pull_session::send_callback;
    handler.rtpport = &rtsp_pull_session::rtp_port_callback;
    handler.ondescribe = &rtsp_pull_session::describe_callback;
    handler.onsetup = &rtsp_pull_session::setup_callback;
    handler.onplay = &rtsp_pull_session::play_callback;
    handler.onpause = &rtsp_pull_session::pause_callback;
    handler.onteardown = &rtsp_pull_session::teardown_callback;
    handler.onrtp = &rtsp_pull_session::rtp_callback;

    auto* client = rtsp_client_create(
        url_.c_str(), username_.empty() ? nullptr : username_.c_str(), password_.empty() ? nullptr : password_.c_str(), &handler, this);
    if (client == nullptr)
    {
        shutdown();
        return;
    }
    client_ = client;

    spdlog::info("rtsp input connected stream {}", stream_name_);
    bool stop = rtsp_client_describe(client_) != 0;
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (!stop)
    {
        const auto bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            break;
        }
        if (rtsp_client_input(client_, buffer.data(), bytes) != 0)
        {
            break;
        }
    }

    client_ = nullptr;
    rtsp_client_destroy(client);
    shutdown();
}

void rtsp_pull_session::write(std::span<const std::uint8_t> data)
{
    if (!transport_ || data.empty())
    {
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context write_yield) { self->run_write(write_yield); }, boost::asio::detached);
    }
}

void rtsp_pull_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        const auto started_at = std::chrono::steady_clock::now();
        static_cast<void>(transport_->write(*data, yield, error));
        if (error)
        {
            shutdown();
            return;
        }

        write_queue_.pop_front();
        if (std::chrono::steady_clock::now() - started_at > slow_write_timeout)
        {
            shutdown();
            return;
        }
    }
}

int rtsp_pull_session::on_describe(const char* sdp, int length)
{
    spdlog::debug("rtsp input describe {}", stream_name_);
    return rtsp_client_setup(client_, sdp, length);
}

int rtsp_pull_session::on_setup(int timeout, std::int64_t)
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

        const char* media_name = media_type == SDP_M_MEDIA_VIDEO ? "video" : (media_type == SDP_M_MEDIA_AUDIO ? "audio" : nullptr);
        const auto* encoding = rtsp_client_get_media_encoding(client_, media);
        const auto* fmtp = rtsp_client_get_media_fmtp(client_, media);
        const auto rate = rtsp_client_get_media_rate(client_, media);
        const auto payload = rtsp_client_get_media_payload(client_, media);
        auto*& demuxer = demuxers_[static_cast<std::size_t>(media)];
        demuxer = rtsp_demuxer_create(media, 500, &rtsp_pull_session::packet_callback, this);
        if (demuxer == nullptr || rtsp_demuxer_add_payload(demuxer, rate, payload, encoding, fmtp) != 0 ||
            rtsp_demuxer_set_info(demuxer, stream_name_.c_str(), rtcp_name) != 0)
        {
            return -1;
        }

        const auto id = media_type == SDP_M_MEDIA_VIDEO ? video_track_id : audio_track_id;
        const auto track = rtsp_sdp_track_from_format(media_name, payload, rate, encoding, fmtp, id);

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

    std::uint64_t npt{};
    return rtsp_client_play(client_, &npt, nullptr);
}

void rtsp_pull_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    const auto media = static_cast<std::size_t>(channel / 2U);
    const bool rtcp = (channel % 2U) != 0U;
    if (media >= demuxers_.size() || demuxers_[media] == nullptr || data == nullptr || bytes < (rtcp ? 4U : 12U))
    {
        return;
    }
    if (rtcp)
    {
        static_cast<void>(rtsp_demuxer_input(demuxers_[media], data, static_cast<int>(bytes)));
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
        startup_timer_.cancel();
        schedule_keepalive();
        schedule_rtcp();
        static_cast<void>(try_initialize_tracks());
        if (!tracks_initialized_)
        {
            startup_timer_.expires_after(initial_tracks_timeout_);
            const auto self = shared_from_this();
            startup_timer_.async_wait(
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
}

int rtsp_pull_session::on_demuxed_packet(avpacket_t* packet)
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
    else if (packet->stream->codecid == AVCODEC_AUDIO_AAC || packet->stream->codecid == AVCODEC_AUDIO_OPUS ||
             packet->stream->codecid == AVCODEC_AUDIO_G711A || packet->stream->codecid == AVCODEC_AUDIO_G711U)
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

bool rtsp_pull_session::update_track_from_packet(const avpacket_t& packet)
{
    const auto& input = *packet.stream;
    auto track = media_track_from_avstream_config(input, video_track_id, audio_track_id);
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

    if (std::chrono::steady_clock::now() >= startup_timer_.expiry())
    {
        shutdown();
        return changed;
    }
    return try_initialize_tracks() || changed;
}

bool rtsp_pull_session::try_initialize_tracks()
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
    if (!registry::instance().add(stream_))
    {
        spdlog::warn("rtsp input duplicate stream {}", stream_name_);
        shutdown();
        return true;
    }
    startup_timer_.cancel();
    spdlog::info("rtsp input tracks ready audio {}", expected_audio_);
    return true;
}

}    // namespace media_server
