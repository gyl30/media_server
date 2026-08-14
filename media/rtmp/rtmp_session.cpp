#include "media/rtmp/rtmp_session.h"

#include "media/codec/codec_utils.h"
#include "media/rtmp/rtmp_timestamp.h"
#include <spdlog/spdlog.h>

extern "C"
{
#include "amf0.h"
#include "flv-demuxer.h"
#include "flv-proto.h"
#include "rtmp-server.h"
}

#include <boost/asio/post.hpp>

#include <array>
#include <utility>

namespace media_server
{

namespace
{
constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;
}    // namespace

rtmp_session::rtmp_session(std::shared_ptr<tcp_connection> connection, stream_registry& registry)
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

void rtmp_session::startup()
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
        shutdown();
        return;
    }

    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code error, std::span<const std::uint8_t> data) { self->on_tcp_read(error, data); },
        [self](boost::system::error_code error, std::size_t write_size) { self->on_tcp_write(error, write_size); });
}

void rtmp_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_ || role_ != role::player || !output_muxer_)
    {
        return;
    }

    apply_tracks(tracks);
    if (!closed_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void rtmp_session::on_read(media_read_batch batch)
{
    if (closed_ || role_ != role::player || !output_muxer_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    apply_tracks(batch.tracks);
    if (closed_)
    {
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto track = reader_tracks_.find(entry.frame.track);
        if (track == reader_tracks_.end() || track->second.config_version != entry.config_version)
        {
            continue;
        }

        if (waiting_for_key_frame_)
        {
            if (track->second.kind != media_kind::video || !entry.frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }
        output_muxer_->on_frame(entry.frame);
    }

    reader_handle().async_read(reader_cursor_);
}

void rtmp_session::on_end() { shutdown(); }

void rtmp_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return;
    }

    bool video_changed = false;
    if (track_revision_ != 0)
    {
        for (const auto& track : tracks->tracks)
        {
            const auto current = reader_tracks_.find(track.id);
            if (current != reader_tracks_.end() && track.kind == media_kind::video &&
                current->second.config_version != track.config_version)
            {
                video_changed = true;
            }
        }
    }

    reader_tracks_.clear();
    for (const auto& track : tracks->tracks)
    {
        reader_tracks_.emplace(track.id, track);
        output_muxer_->on_track(track);
    }
    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
}

int rtmp_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_session*>(param);
    self->connection_->write(header, header_bytes);
    self->connection_->write(payload, payload_bytes);
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_session::play_callback(void* param, const char* app, const char* stream, double, double, std::uint8_t)
{
    return static_cast<rtmp_session*>(param)->on_play(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::pause_callback(void*, int, std::uint32_t) { return -1; }

int rtmp_session::seek_callback(void*, std::uint32_t) { return -1; }

int rtmp_session::publish_callback(void* param, const char* app, const char* stream, const char*)
{
    return static_cast<rtmp_session*>(param)->on_publish(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (self->demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(self->demuxer_, FLV_TYPE_VIDEO, data, bytes, timestamp);
}

int rtmp_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (self->demuxer_ == nullptr)
    {
        return -1;
    }
    return flv_demuxer_input(self->demuxer_, FLV_TYPE_AUDIO, data, bytes, timestamp);
}

int rtmp_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    return static_cast<rtmp_session*>(param)->on_script(
        std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes));
}

int rtmp_session::duration_callback(void*, const char*, const char*, double* duration)
{
    if (duration != nullptr)
    {
        *duration = 0.0;
    }
    return 0;
}

int rtmp_session::demux_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (data == nullptr)

    {
        return -1;
    }
    return static_cast<rtmp_session*>(param)->on_flv_demux(
        codec, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes), pts, dts, flags);
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
        [this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            if (server_ == nullptr)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                static_cast<void>(rtmp_server_send_video(server_, data.data(), data.size(), timestamp));
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                static_cast<void>(rtmp_server_send_audio(server_, data.data(), data.size(), timestamp));
            }
        });

    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(),
                      [self]()
                      {
                          if (self->closed_ || self->server_ == nullptr || !self->stream_)
                          {
                              return;
                          }

                          if (rtmp_server_start(self->server_, 0, nullptr) != 0)
                          {
                              self->shutdown();
                              return;
                          }

                          self->reader_ = self->stream_->add_reader(self, self->connection_->socket().get_executor());
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
    stream_ = std::make_shared<media_stream>(stream_name_, connection_->socket().get_executor());
    demuxer_ = flv_demuxer_create(&rtmp_session::demux_callback, this);
    if (demuxer_ == nullptr)
    {
        stream_.reset();
        return -1;
    }
    if (!registry_.add(stream_))
    {
        spdlog::warn("rtmp publish duplicate stream {}", stream_name_);
        flv_demuxer_destroy(demuxer_);
        demuxer_ = nullptr;
        stream_.reset();
        return -1;
    }

    role_ = role::publisher;
    spdlog::info("rtmp publish {}", stream_name_);
    return 0;
}

int rtmp_session::on_script(std::span<const std::uint8_t> data)
{
    if (role_ != role::publisher)
    {
        return 0;
    }
    if (data.empty())
    {
        return 0;
    }

    const auto* end = data.data() + data.size();
    std::array<char, 16> name{};
    if (data.front() != AMF_STRING)
    {
        return 0;
    }
    const auto* values = AMFReadString(data.data() + 1, end, 0, name.data(), name.size());
    if (values == nullptr)
    {
        return -1;
    }
    if (std::string_view(name.data()) != "onMetaData")
    {
        return 0;
    }

    double audio_codec{};
    std::array<amf_object_item_t, 1> properties{
        amf_object_item_t{AMF_NUMBER, "audiocodecid", &audio_codec, sizeof(audio_codec)},
    };
    std::array<amf_object_item_t, 1> metadata{
        amf_object_item_t{AMF_OBJECT, "metadata", properties.data(), properties.size()},
    };
    if (amf_read_items(values, end, metadata.data(), metadata.size()) == nullptr)
    {
        return -1;
    }

    const bool audio = audio_codec != 0.0;
    if ((metadata_received_ && expected_audio_ != audio) || (initial_audio_track_ && !audio))
    {
        return -1;
    }

    expected_audio_ = audio;
    metadata_received_ = true;
    try_initialize_tracks();
    return 0;
}

int rtmp_session::on_flv_demux(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags)
{
    if (role_ != role::publisher || !stream_)

    {
        return -1;
    }

    if (codec == FLV_VIDEO_AVCC || codec == FLV_VIDEO_HVCC)
    {
        const auto video_codec = codec == FLV_VIDEO_AVCC ? codec_id::h264 : codec_id::h265;
        if (!tracks_initialized_ && initial_video_track_ && initial_video_track_->codec != video_codec)
        {
            spdlog::warn("rtmp input video codec change {} {}", to_string(initial_video_track_->codec), to_string(video_codec));
            return -1;
        }
        if (tracks_initialized_)
        {
            for (const auto& track : stream_->tracks())
            {
                if (track.id == video_track_id && track.codec != video_codec)
                {
                    spdlog::warn("rtmp input video codec change {} {}", to_string(track.codec), to_string(video_codec));
                    return -1;
                }
            }
        }

        auto config = codec == FLV_VIDEO_AVCC ? h264_avcc_to_annex_b(data) : h265_hvcc_to_annex_b(data);
        if (config.empty())
        {
            return -1;
        }
        media_track track{
            .id = video_track_id,
            .kind = media_kind::video,
            .codec = video_codec,
            .clock_rate = 90'000,
            .channel_count = 0,
            .codec_config = std::move(config),
        };
        if (!tracks_initialized_)
        {
            initial_video_track_ = std::move(track);
            try_initialize_tracks();
            return 0;
        }
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track video {}", to_string(video_codec));
        }
        return 0;
    }

    if (codec == FLV_AUDIO_ASC)
    {
        if (metadata_received_ && !expected_audio_)
        {
            return -1;
        }
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
        if (!tracks_initialized_)
        {
            initial_audio_track_ = std::move(track);
            try_initialize_tracks();
            return 0;
        }
        if (stream_->update_track(std::move(track)))
        {
            spdlog::info("rtmp input track audio aac sample_rate {} channels {}", config->sample_rate, config->channel_count);
        }
        return 0;
    }

    track_id id{};
    if (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265)
    {
        id = video_track_id;
    }
    else if (codec == FLV_AUDIO_AAC)
    {
        id = audio_track_id;
    }
    else
    {
        return 0;
    }

    const auto dts_ms = unwrap_rtmp_timestamp(dts, timestamp_);
    const auto pts_ms = dts_ms + rtmp_timestamp_delta(pts, dts);

    auto payload = std::make_shared<const std::vector<std::uint8_t>>(data.begin(), data.end());
    media_frame frame{
        .track = id,
        .dts_ns = milliseconds_to_ns(dts_ms),
        .pts_ns = milliseconds_to_ns(pts_ms),
        .key_frame = (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265) && flags != 0,
        .payload = std::move(payload),
    };
    stream_->publish(std::move(frame));
    return 0;
}

void rtmp_session::try_initialize_tracks()
{
    if (tracks_initialized_ || !metadata_received_ || !initial_video_track_ || (expected_audio_ && !initial_audio_track_))
    {
        return;
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
    if (tracks_initialized_)
    {
        spdlog::info("rtmp input tracks ready audio {}", expected_audio_);
    }
}

void rtmp_session::on_tcp_read(boost::system::error_code error, std::span<const std::uint8_t> data)
{
    if (error)
    {
        shutdown();
        return;
    }
    if (server_ == nullptr || closed_)
    {
        return;
    }
    if (rtmp_server_input(server_, data.data(), data.size()) != 0)
    {
        shutdown();
    }
}

void rtmp_session::on_tcp_write(boost::system::error_code error, std::size_t write_size)
{
    static_cast<void>(write_size);
    if (error)
    {
        shutdown();
    }
}

void rtmp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(), [self]() { self->safe_shutdown(); });
}

void rtmp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (role_ == role::publisher && stream_)
    {
        registry_.remove(*stream_);
        stream_->end();
    }
    reader_.remove();
    reader_ = {};
    reader_cursor_.reset();
    reader_tracks_.clear();
    track_revision_ = 0;
    waiting_for_key_frame_ = false;
    connection_->shutdown();
    stream_.reset();
    output_muxer_.reset();

    if (demuxer_ != nullptr)
    {
        flv_demuxer_destroy(demuxer_);
        demuxer_ = nullptr;
    }
    if (server_ != nullptr)
    {
        rtmp_server_destroy(server_);
        server_ = nullptr;
    }
    spdlog::debug("rtmp shutdown {}", stream_name_);
}

std::string rtmp_session::make_stream_name(std::string_view app, std::string_view stream)
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
