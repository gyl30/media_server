#include <array>
#include <chrono>
#include <vector>
#include <utility>
#include <optional>
#include <algorithm>
#include <string_view>

#include <spdlog/spdlog.h>
#include <boost/url/url.hpp>
#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/detached.hpp>

#include "media/rtsp/rtsp_sdp.h"
#include "media/rtsp/rtsp_event.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_media.h"
#include "media/rtsp/rtsp_pull_session.h"

extern "C"
{
#include "sdp.h"
#include "rtp-profile.h"
#include "rtsp-client.h"
}

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
constexpr std::size_t max_media_count = 2;

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
                                     std::string stream_id,
                                     std::string source_id,
                                     std::string stream_name,
                                     std::string url,
                                     std::string username,
                                     std::string password,
                                     std::chrono::milliseconds establishment_timeout,
                                     std::chrono::milliseconds initial_tracks_timeout,
                                     std::size_t max_write_queue_bytes)
    : worker_(worker),
      stream_id_(std::move(stream_id)),
      source_id_(std::move(source_id)),
      stream_name_(std::move(stream_name)),
      url_(std::move(url)),
      username_(std::move(username)),
      password_(std::move(password)),
      resolver_(worker_.io()),
      connect_socket_(worker_.io()),
      startup_timer_(worker_.io()),
      keepalive_timer_(worker_.io()),
      rtcp_timer_(worker_.io()),
      write_queue_(max_write_queue_bytes),
      establishment_timeout_(establishment_timeout),
      initial_tracks_timeout_(initial_tracks_timeout)
{
}

rtsp_pull_session::~rtsp_pull_session() = default;

bool rtsp_pull_session::valid_url(std::string_view url) { return parse_url(url).has_value(); }

std::string_view rtsp_pull_session::stream_id() const noexcept { return stream_id_; }

std::string_view rtsp_pull_session::source_id() const noexcept { return source_id_; }

std::string_view rtsp_pull_session::stream_name() const noexcept { return stream_name_; }

bool rtsp_pull_session::startup()
{
    if (closed_ || started_)
    {
        return false;
    }
    const auto parsed = parse_url(url_);
    if (!parsed)
    {
        return false;
    }

    url_ = parsed->request_url;
    started_ = true;

    record_establishment_progress();
    schedule_establishment_timeout();

    const auto self = shared_from_this();
    boost::asio::spawn(
        worker_.io(),
        [self, host = parsed->host, port = parsed->port](boost::asio::yield_context yield) { self->run(host, port, yield); },
        boost::asio::detached);
    rtsp_event::report_source(event_state::starting, stream_id_, stream_name_, source_id_, "resolving");
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

            spdlog::warn("rtsp pull establishment timeout stream {}", self->stream_name_);
            if (self->started_)
            {
                rtsp_event::report_source(event_state::timeout, self->stream_id_, self->stream_name_, self->source_id_, {}, "establishment_timeout");
            }
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
                if (self->started_)
                {
                    rtsp_event::report_source(
                        event_state::protocol_error, self->stream_id_, self->stream_name_, self->source_id_, {}, "keepalive_failed");
                }
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
            for (std::size_t media = 0; media < self->media_count_; ++media)
            {
                const auto bytes = self->media_->generate_rtcp(media, buffer);
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
    write_queue_.stop();
    if (started_)
    {
        rtsp_event::report_source(event_state::stopped, stream_id_, stream_name_, source_id_);
    }
    started_ = false;
    media_started_ = false;
    stream_registry::instance().remove_receiver_session(stream_name_, *this);
    if (media_)
    {
        media_->shutdown();
        media_.reset();
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
    spdlog::debug("rtsp pull shutdown {}", stream_name_);
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
        spdlog::debug("rtsp pull ignore media {} encoding {}", media, encoding != nullptr ? encoding : "");
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
    return self->media_->set_rtp_info(static_cast<std::size_t>(media), static_cast<std::uint16_t>(info[0].seq), info[0].time);
}

int rtsp_pull_session::pause_callback(void*) { return 0; }

int rtsp_pull_session::teardown_callback(void*) { return 0; }

void rtsp_pull_session::rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    static_cast<rtsp_pull_session*>(param)->on_rtp(channel, data, bytes);
}

std::optional<rtsp_pull_session::parsed_url> rtsp_pull_session::parse_url(std::string_view url)
{
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed || parsed->scheme() != "rtsp" || !parsed->has_authority() || parsed->has_userinfo())
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

    parsed_url result;
    result.request_url = std::string(parsed->buffer());
    result.host = std::string(host);
    result.port = parsed->has_port() ? parsed->port_number() : static_cast<std::uint16_t>(554);
    return result;
}

void rtsp_pull_session::run(std::string host, std::uint16_t port, boost::asio::yield_context yield)
{
    boost::system::error_code error;
    const auto endpoints = resolver_.async_resolve(host, std::to_string(port), yield[error]);
    if (closed_)
    {
        return;
    }
    if (error)
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, error.message());
        }
        shutdown();
        return;
    }

    record_establishment_progress();
    boost::asio::async_connect(connect_socket_, endpoints, yield[error]);
    if (closed_)
    {
        return;
    }
    if (error)
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, error.message());
        }
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
        url_.c_str(), username_.empty() ? nullptr : username_.c_str(), username_.empty() ? nullptr : password_.c_str(), &handler, this);
    if (client == nullptr)
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, "rtsp_client_create_failed");
        }
        shutdown();
        return;
    }
    client_ = client;

    spdlog::info("rtsp pull connected stream {}", stream_name_);
    bool stop = rtsp_client_describe(client_) != 0;
    if (stop)
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::protocol_error, stream_id_, stream_name_, source_id_, {}, "describe_failed");
        }
        shutdown();
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (!stop)
    {
        const auto bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, error.message());
            shutdown();
            break;
        }
        if (rtsp_client_input(client_, buffer.data(), bytes) != 0)
        {
            if (started_)
            {
                rtsp_event::report_source(event_state::protocol_error, stream_id_, stream_name_, source_id_, {}, "rtsp_input_failed");
            }
            shutdown();
            break;
        }
    }

    client_ = nullptr;
    rtsp_client_destroy(client);
    if (!closed_)
    {
        shutdown();
    }
}

void rtsp_pull_session::write(std::span<const std::uint8_t> data)
{
    if (closed_ || !transport_ || data.empty())
    {
        return;
    }

    const auto result = write_queue_.enqueue(std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()));
    if (result == tcp_write_enqueue_result::overflow)
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, "write_queue_full");
        }
        shutdown();
        return;
    }
    if (result == tcp_write_enqueue_result::stopped)
    {
        return;
    }

    if (result == tcp_write_enqueue_result::start_writer)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context write_yield) { self->run_write(write_yield); }, boost::asio::detached);
    }
}

void rtsp_pull_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (closed_ || write_queue_.stopped() || write_queue_.empty())
        {
            return;
        }

        const auto result = write_queue_.write_one(*transport_, yield);
        if (result.error)
        {
            rtsp_event::report_source(event_state::runtime_error, stream_id_, stream_name_, source_id_, {}, result.error.message());
            shutdown();
            return;
        }
    }
}

int rtsp_pull_session::on_describe(const char* sdp, int length)
{
    spdlog::debug("rtsp pull describe {}", stream_name_);
    return rtsp_client_setup(client_, sdp, length);
}

int rtsp_pull_session::on_setup(int timeout, std::int64_t)
{
    const auto keepalive_seconds = timeout > 0 ? std::max(timeout / 2, 1) : 30;
    keepalive_interval_ = std::chrono::seconds(keepalive_seconds);

    const auto media_count = rtsp_client_media_count(client_);
    if (media_count < 0 || static_cast<std::size_t>(media_count) > max_media_count)
    {
        return -1;
    }

    bool expected_video = false;
    std::vector<rtsp_pull_track_description> descriptions;
    descriptions.reserve(static_cast<std::size_t>(media_count));
    for (int media = 0; media < media_count; ++media)
    {
        const auto media_type = rtsp_client_get_media_type(client_, media);
        expected_video = expected_video || media_type == SDP_M_MEDIA_VIDEO;

        const char* media_name = media_type == SDP_M_MEDIA_VIDEO ? "video" : (media_type == SDP_M_MEDIA_AUDIO ? "audio" : nullptr);
        const auto* encoding = rtsp_client_get_media_encoding(client_, media);
        const auto* fmtp = rtsp_client_get_media_fmtp(client_, media);
        const auto rate = rtsp_client_get_media_rate(client_, media);
        const auto payload = rtsp_client_get_media_payload(client_, media);
        const auto id = media_type == SDP_M_MEDIA_VIDEO ? video_track_id : audio_track_id;
        descriptions.push_back(rtsp_pull_track_description{
            .kind = media_type == SDP_M_MEDIA_VIDEO ? media_kind::video : media_kind::audio,
            .clock_rate = rate,
            .payload_type = payload,
            .encoding = encoding != nullptr ? encoding : "",
            .fmtp = fmtp != nullptr ? fmtp : "",
            .initial_track = rtsp_sdp_track_from_format(media_name, payload, rate, encoding, fmtp, id),
        });
    }
    if (!expected_video)
    {
        return -1;
    }

    auto media = std::make_unique<rtsp_pull_media>(worker_, stream_name_, std::move(descriptions));
    if (!media->startup())
    {
        return -1;
    }
    media_ = std::move(media);
    media_count_ = static_cast<std::size_t>(media_count);

    std::uint64_t npt{};
    return rtsp_client_play(client_, &npt, nullptr);
}

void rtsp_pull_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    if (closed_)
    {
        return;
    }
    const auto media = static_cast<std::size_t>(channel / 2U);
    const bool rtcp = (channel % 2U) != 0U;
    if (media >= media_count_ || data == nullptr || bytes < (rtcp ? 4U : 12U))
    {
        return;
    }

    if (!rtcp)
    {
        if (!media_started_)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= last_establishment_progress_ + establishment_timeout_)
            {
                spdlog::warn("rtsp pull establishment timeout stream {}", stream_name_);
                if (started_)
                {
                    rtsp_event::report_source(event_state::timeout, stream_id_, stream_name_, source_id_, {}, "establishment_timeout");
                }
                shutdown();
                return;
            }
            media_started_ = true;
            startup_timer_.cancel();
            schedule_keepalive();
            schedule_rtcp();
            if (!media_->tracks_initialized())
            {
                startup_timer_.expires_after(initial_tracks_timeout_);
                const auto self = shared_from_this();
                startup_timer_.async_wait(
                    [self](const boost::system::error_code& error)
                    {
                        if (error || self->closed_ || !self->media_ || self->media_->tracks_initialized())
                        {
                            return;
                        }
                        spdlog::warn("rtsp pull initial tracks timeout stream {}", self->stream_name_);
                        if (self->started_)
                        {
                            rtsp_event::report_source(
                                event_state::timeout, self->stream_id_, self->stream_name_, self->source_id_, {}, "initial_tracks_timeout");
                        }
                        self->shutdown();
                    });
            }
        }
        else if (!media_->tracks_initialized() && std::chrono::steady_clock::now() >= startup_timer_.expiry())
        {
            if (started_)
            {
                rtsp_event::report_source(event_state::timeout, stream_id_, stream_name_, source_id_, {}, "initial_tracks_timeout");
            }
            shutdown();
            return;
        }
    }

    const bool tracks_initialized = media_->tracks_initialized();
    if (!media_->input_packet(channel, std::span{static_cast<const std::uint8_t*>(data), bytes}))
    {
        if (started_)
        {
            rtsp_event::report_source(event_state::protocol_error, stream_id_, stream_name_, source_id_, {}, "media_input_failed");
        }
        shutdown();
        return;
    }
    if (!rtcp && media_->tracks_initialized())
    {
        startup_timer_.cancel();
        if (!tracks_initialized)
        {
            rtsp_event::report_source(event_state::streaming, stream_id_, stream_name_, source_id_, "streaming");
        }
    }
}

}    // namespace media_server
