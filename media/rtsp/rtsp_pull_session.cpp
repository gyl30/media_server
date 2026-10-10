#include <array>
#include <chrono>
#include <vector>
#include <utility>
#include <optional>
#include <algorithm>
#include <string_view>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>

#include "media/rtsp/rtsp_sdp.h"
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

rtsp_pull_session::rtsp_pull_session(worker_context& worker, rtsp_pull_config config)
    : worker_(worker),
      config_(std::move(config)),
      resolver_(worker_.io()),
      connect_socket_(worker_.io()),
      rtcp_timer_(worker_.io()),
      idle_timer_(worker_.io())
{
}

rtsp_pull_session::~rtsp_pull_session() = default;

void rtsp_pull_session::startup()
{
    const auto self = shared_from_this();
    // 连接、协商和收流都受同一空闲时间约束，上游不再发送 RTP 时结束会话。
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("rtsp pull idle timeout {}", config_.stream_id);
                          shutdown();
                      });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void rtsp_pull_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtsp_pull_session::schedule_rtcp()
{
    rtcp_timer_.expires_after(std::chrono::seconds(1));
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || !self->transport_ || !self->media_)
            {
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            for (std::size_t media = 0; media < max_media_count; ++media)
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
                self->transport_->write(std::move(packet));
            }
            self->schedule_rtcp();
        });
}

void rtsp_pull_session::safe_shutdown()
{
    idle_timer_.stop();
    session_registry::instance().remove_receiver_session(config_.stream_id, *this);
    if (media_)
    {
        media_->shutdown();
        media_.reset();
    }
    rtcp_timer_.cancel();
    resolver_.cancel();
    boost::system::error_code error;
    connect_socket_.close(error);
    if (transport_)
    {
        transport_->shutdown();
    }
    spdlog::debug("rtsp pull shutdown {}", config_.stream_id);
}

int rtsp_pull_session::send_callback(void* param, const char*, const void* request, std::size_t bytes)
{
    auto* self = static_cast<rtsp_pull_session*>(param);
    if (!self->transport_)
    {
        return -1;
    }
    self->transport_->write(std::span{static_cast<const std::uint8_t*>(request), bytes});
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

int rtsp_pull_session::setup_callback(void* param, int, std::int64_t)
{
    return static_cast<rtsp_pull_session*>(param)->on_setup();
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

void rtsp_pull_session::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    const auto endpoints = resolver_.async_resolve(config_.host, std::to_string(config_.port), yield[error]);
    if (yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    if (error)
    {
        shutdown();
        return;
    }

    boost::asio::async_connect(connect_socket_, endpoints, yield[error]);
    if (yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    if (error)
    {
        shutdown();
        return;
    }

    transport_ = std::make_shared<tcp_transport>(std::move(connect_socket_));
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [self](boost::system::error_code write_error, std::size_t)
        {
            if (write_error)
            {
                self->shutdown();
            }
        });

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
        config_.url.c_str(), config_.username.empty() ? nullptr : config_.username.c_str(),
        config_.username.empty() ? nullptr : config_.password.c_str(), &handler, this);
    if (client == nullptr)
    {
        shutdown();
        return;
    }
    client_ = client;

    spdlog::info("rtsp pull connected stream {}", config_.stream_id);
    run_read(client, yield);

    client_ = nullptr;
    rtsp_client_destroy(client);
    shutdown();
}

void rtsp_pull_session::run_read(rtsp_client_t* client, boost::asio::yield_context yield)
{
    if (rtsp_client_describe(client) != 0)
    {
        return;
    }

    boost::system::error_code error;
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        const auto bytes = transport_->read(buffer, yield, error);
        if (error)
        {
            return;
        }
        idle_timer_.touch();
        if (rtsp_client_input(client, buffer.data(), bytes) != 0)
        {
            return;
        }
    }
}

int rtsp_pull_session::on_describe(const char* sdp, int length)
{
    spdlog::debug("rtsp pull describe {}", config_.stream_id);
    return rtsp_client_setup(client_, sdp, length);
}

int rtsp_pull_session::on_setup()
{
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

    // 失败返回 -1 结束读循环，media_ 由 safe_shutdown 清理。
    media_ = std::make_unique<rtsp_pull_media>(worker_, config_.stream_id);
    if (!media_->startup(std::move(descriptions)))
    {
        return -1;
    }

    std::uint64_t npt{};
    const auto result = rtsp_client_play(client_, &npt, nullptr);
    if (result == 0)
    {
        schedule_rtcp();
    }
    return result;
}

void rtsp_pull_session::on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    if (!media_)
    {
        return;
    }
    const bool rtcp = (channel % 2U) != 0U;
    if (data == nullptr || bytes < (rtcp ? 4U : 12U))
    {
        return;
    }

    if (!media_->input_packet(channel, std::span{static_cast<const std::uint8_t*>(data), bytes}))
    {
        shutdown();
        return;
    }

}

}    // namespace media_server
