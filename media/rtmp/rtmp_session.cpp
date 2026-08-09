#include "media/rtmp/rtmp_session.h"

#include "media/codec/codec_utils.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "flv-demuxer.h"
#include "flv-proto.h"
#include "rtmp-server.h"
}

#include <boost/asio/post.hpp>

#include <utility>

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
}

rtmp_session::rtmp_session(
    std::shared_ptr<tcp_connection> connection,
    stream_registry& registry)
    : connection_(std::move(connection)), registry_(registry)
{
}

rtmp_session::~rtmp_session()
{
    if (demuxer_ != nullptr)
    {
        flv_demuxer_destroy(demuxer_);
    }
    if (server_ != nullptr)
    {
        rtmp_server_destroy(server_);
    }
}

void rtmp_session::start()
{
    rtmp_server_handler_t handler{};
    handler.send = &rtmp_session::send_callback;
    handler.onplay = &rtmp_session::play_callback;
    handler.onpause = &rtmp_session::pause_callback;
    handler.onseek = &rtmp_session::seek_callback;
    handler.onpublish = &rtmp_session::publish_callback;
    handler.onvideo = &rtmp_session::video_callback;
    handler.onaudio = &rtmp_session::audio_callback;
    handler.onscript = &rtmp_session::script_callback;
    handler.ongetduration = &rtmp_session::duration_callback;

    server_ = rtmp_server_create(this, &handler);
    if (server_ == nullptr)
    {
        close();
        return;
    }

    const auto self = shared_from_this();
    connection_->start(
        [self](std::span<const std::uint8_t> data) { self->on_read(data); },
        [self]() { self->on_close(); });
}

void rtmp_session::on_track(const media_track& track)
{
    if (output_muxer_)
    {
        output_muxer_->on_track(track);
    }
}

void rtmp_session::on_frame(const media_frame& frame)
{
    if (output_muxer_)
    {
        output_muxer_->on_frame(frame);
    }
}

void rtmp_session::on_end()
{
    close();
}

int rtmp_session::send_callback(
    void* param,
    const void* header,
    std::size_t header_bytes,
    const void* payload,
    std::size_t payload_bytes)
    {

    auto* self = static_cast<rtmp_session*>(param);
    self->connection_->write(header, header_bytes);
    self->connection_->write(payload, payload_bytes);
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_session::play_callback(
    void* param,
    const char* app,
    const char* stream,
    double,
    double,
    std::uint8_t)
    {
    return static_cast<rtmp_session*>(param)->on_play(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::pause_callback(void*, int, std::uint32_t)
{
    return 0;
}

int rtmp_session::seek_callback(void*, std::uint32_t)
{
    return 0;
}

int rtmp_session::publish_callback(
    void* param,
    const char* app,
    const char* stream,
    const char*)
    {
    return static_cast<rtmp_session*>(param)->on_publish(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::video_callback(
    void* param,
    const void* data,
    std::size_t bytes,
    std::uint32_t timestamp)
    {
    auto* self = static_cast<rtmp_session*>(param);
    if (self->demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(self->demuxer_, FLV_TYPE_VIDEO, data, bytes, timestamp);
}

int rtmp_session::audio_callback(
    void* param,
    const void* data,
    std::size_t bytes,
    std::uint32_t timestamp)
    {
    auto* self = static_cast<rtmp_session*>(param);
    if (self->demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(self->demuxer_, FLV_TYPE_AUDIO, data, bytes, timestamp);
}

int rtmp_session::script_callback(
    void*,
    const void*,
    std::size_t,
    std::uint32_t)
    {
    return 0;
}

int rtmp_session::duration_callback(
    void*,
    const char*,
    const char*,
    double* duration)
    {
    if (duration != nullptr)
    {
        *duration = 0.0;
    }
    return 0;
}

int rtmp_session::demux_callback(
    void* param,
    int codec,
    const void* data,
    std::size_t bytes,
    std::uint32_t pts,
    std::uint32_t dts,
    int flags)
    {

    if (data == nullptr)

    {
        return -1;
    }
    return static_cast<rtmp_session*>(param)->on_flv_demux(
        codec,
        std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes),
        pts,
        dts,
        flags);
}

int rtmp_session::on_play(std::string app, std::string stream)
{
    if (role_ != role::none)
    {
        return -1;
    }

    stream_name_ = make_stream_name(app, stream);
    stream_ = registry_.find(stream_name_);
    if (!stream_)
    {
        spdlog::warn("rtmp play stream not found {}", stream_name_);
        return -1;
    }

    role_ = role::player;
    output_muxer_ = std::make_unique<flv_output_muxer>(
        [this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp) {
            if (server_ == nullptr)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                static_cast<void>(rtmp_server_send_video(server_, data.data(), data.size(), timestamp));
            } else if (type == FLV_TYPE_AUDIO)
            {
                static_cast<void>(rtmp_server_send_audio(server_, data.data(), data.size(), timestamp));
            }
        });

    const auto self = shared_from_this();
    boost::asio::post(
        connection_->socket().get_executor(),
        [self]() {
            if (self->closed_ || self->server_ == nullptr || !self->stream_)
            {
                return;
            }

            if (rtmp_server_start(self->server_, 0, nullptr) != 0)
            {
                self->close();
                return;
            }

            if (!self->stream_->add_sink(self))
            {
                self->close();
                return;
            }

            spdlog::info("rtmp play {}", self->stream_name_);
        });

    return RTMP_SERVER_ASYNC_START;
}

int rtmp_session::on_publish(std::string app, std::string stream)
{
    if (role_ != role::none)
    {
        return -1;
    }

    stream_name_ = make_stream_name(app, stream);
    stream_ = std::make_shared<media_stream>(stream_name_);
    if (!registry_.add(stream_))
    {
        spdlog::warn("rtmp publish duplicate stream {}", stream_name_);
        stream_.reset();
        return -1;
    }

    demuxer_ = flv_demuxer_create(&rtmp_session::demux_callback, this);
    if (demuxer_ == nullptr)
    {
        registry_.remove(*stream_);
        stream_.reset();
        return -1;
    }

    role_ = role::publisher;
    spdlog::info("rtmp publish {}", stream_name_);
    return 0;
}

int rtmp_session::on_flv_demux(
    int codec,
    std::span<const std::uint8_t> data,
    std::uint32_t pts,
    std::uint32_t dts,
    int flags)
    {

    if (role_ != role::publisher || !stream_)

    {
        return -1;
    }

    if (codec == FLV_VIDEO_AVCC)

    {
        auto config = h264_avcc_to_annex_b(data);
        if (config.empty())
        {
            return -1;
        }
        media_track track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = codec_id::h264,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = std::move(config),
        };
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track video h264");
        }
        return 0;
    }

    if (codec == FLV_AUDIO_ASC)

    {
        const auto config = parse_aac_asc(data);
        if (!config)
        {
            return -1;
        }
        media_track track{
            .id = audio_track_id,
            .kind = media_kind::audio,
            .codec = codec_id::aac,
            .clock_rate = config->sample_rate,
            .channel_count = config->channel_count,
            .codec_config = {data.begin(), data.end()},
        };
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track audio aac sample_rate {} channels {}", config->sample_rate, config->channel_count);
        }
        return 0;
    }

    track_id id{};
    if (codec == FLV_VIDEO_H264)
    {
        id = video_track_id;
    } else if (codec == FLV_AUDIO_AAC)
    {
        id = audio_track_id;
    }
    else
    {
        return 0;
    }

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(data.begin(), data.end());
    media_frame frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(dts),
        .pts_ns = milliseconds_to_ns(pts),
        .key_frame = codec == FLV_VIDEO_H264 && flags != 0,
        .payload = std::move(payload),
    };
    stream_->publish(std::move(frame));
    return 0;
}

void rtmp_session::on_read(std::span<const std::uint8_t> data)
{
    if (server_ == nullptr || closed_)
    {
        return;
    }
    if (rtmp_server_input(server_, data.data(), data.size()) != 0)
    {
        close();
    }
}

void rtmp_session::on_close()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    if (role_ == role::player && stream_)

    {
        stream_->remove_sink(*this);
    }
    if (role_ == role::publisher && stream_)
    {
        stream_->end();
        registry_.remove(*stream_);
    }
    stream_.reset();
    output_muxer_.reset();
    spdlog::debug("rtmp close {}", stream_name_);
}

void rtmp_session::close()
{
    if (closed_)
    {
        return;
    }
    connection_->close();
}

std::string rtmp_session::make_stream_name(
    std::string_view app,
    std::string_view stream)
    {
    if (app.empty())
    {
        return std::string(stream);
    }
    if (stream.empty())
    {
        return std::string(app);
    }
    return std::string(app) + '/' + std::string(stream);
}

}    // namespace media_server
