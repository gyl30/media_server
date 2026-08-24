#include <random>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/codec/codec_utils.h"
#include "media/gb28181/gb28181_output_media.h"

extern "C"
{
#include "rtsp-muxer.h"
#include "rtp-profile.h"
}

namespace media_server
{

gb28181_output_media::gb28181_output_media(boost::asio::any_io_executor executor,
                                           std::shared_ptr<media_stream> stream,
                                           std::uint8_t payload_type,
                                           std::uint32_t ssrc,
                                           packet_handler on_packet,
                                           end_handler on_end)
    : executor_(std::move(executor)),
      stream_(std::move(stream)),
      payload_type_(payload_type),
      ssrc_(ssrc),
      on_packet_(std::move(on_packet)),
      on_end_(std::move(on_end))
{
}

gb28181_output_media::~gb28181_output_media()
{
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
    }
}

bool gb28181_output_media::supported_tracks(const std::vector<media_track>& tracks)
{
    if (tracks.empty())
    {
        return false;
    }

    std::size_t video_count = 0;
    std::size_t audio_count = 0;
    for (const auto& track : tracks)
    {
        if (track.kind == media_kind::video)
        {
            ++video_count;
            if (track.codec != codec_id::h264 && track.codec != codec_id::h265)
            {
                return false;
            }
        }
        else
        {
            ++audio_count;
            if (track.codec != codec_id::aac && track.codec != codec_id::g711a && track.codec != codec_id::g711u)
            {
                return false;
            }
        }
    }
    return video_count == 1 && audio_count <= 1;
}

bool gb28181_output_media::startup()
{
    if (closed_ || !stream_ || muxer_ != nullptr || !on_packet_ || !create_muxer(stream_->tracks()))
    {
        return false;
    }

    reader_ = stream_->add_reader(shared_from_this(), executor_);
    return true;
}

void gb28181_output_media::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void gb28181_output_media::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_)
    {
        return;
    }
    if (!apply_tracks(tracks))
    {
        if (on_end_)
        {
            on_end_();
        }
        else
        {
            shutdown();
        }
        return;
    }
    reader_handle().async_read(reader_cursor_);
}

void gb28181_output_media::on_read(media_read_batch batch)
{
    if (closed_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    if (!apply_tracks(batch.tracks))
    {
        if (on_end_)
        {
            on_end_();
        }
        else
        {
            shutdown();
        }
        return;
    }

    for (const auto& entry : batch.entries)
    {
        const auto state = tracks_.find(entry.frame.track);
        if (state == tracks_.end() || state->second.config_version != entry.config_version || !entry.frame.payload)
        {
            continue;
        }

        if (waiting_for_key_frame_)
        {
            if (state->second.kind != media_kind::video || !entry.frame.key_frame)
            {
                continue;
            }
            waiting_for_key_frame_ = false;
        }

        const auto result = rtsp_muxer_input(muxer_,
                                             state->second.media_id,
                                             ns_to_milliseconds(entry.frame.pts_ns),
                                             ns_to_milliseconds(entry.frame.dts_ns),
                                             entry.frame.payload->data(),
                                             static_cast<int>(entry.frame.payload->size()),
                                             entry.frame.key_frame ? 1 : 0);
        if (result < 0)
        {
            spdlog::error("gb28181 output mux failed stream {} result {}", stream_->name(), result);
            if (on_end_)
            {
                on_end_();
            }
            else
            {
                shutdown();
            }
            return;
        }
    }

    if (!closed_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void gb28181_output_media::on_end()
{
    if (!closed_ && on_end_)
    {
        on_end_();
    }
}

int gb28181_output_media::muxer_packet_callback(void* param, int, const void* data, int bytes, std::uint32_t, int)
{
    return static_cast<gb28181_output_media*>(param)->on_muxer_packet(data, bytes);
}

void gb28181_output_media::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    reader_.remove();
    reader_ = {};
    reader_cursor_.reset();
    track_revision_ = 0;
    tracks_.clear();
    waiting_for_key_frame_ = true;
    stream_.reset();
    on_packet_ = {};
    on_end_ = {};
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
}

bool gb28181_output_media::create_muxer(const std::vector<media_track>& tracks)
{
    if (!supported_tracks(tracks))
    {
        return false;
    }

    muxer_ = rtsp_muxer_create(&gb28181_output_media::muxer_packet_callback, this);
    if (muxer_ == nullptr)
    {
        return false;
    }

    std::random_device device;
    const auto payload =
        rtsp_muxer_add_payload(muxer_, "RTP/AVP", 90'000, payload_type_, "PS", static_cast<std::uint16_t>(device()), ssrc_, 0, nullptr, 0);
    if (payload < 0)
    {
        return false;
    }

    for (const auto& track : tracks)
    {
        int codec = -1;
        switch (track.codec)
        {
            case codec_id::h264:
                codec = RTP_PAYLOAD_H264;
                break;
            case codec_id::h265:
                codec = RTP_PAYLOAD_H265;
                break;
            case codec_id::aac:
                codec = RTP_PAYLOAD_MP4A;
                break;
            case codec_id::g711a:
                codec = RTP_PAYLOAD_PCMA;
                break;
            case codec_id::g711u:
                codec = RTP_PAYLOAD_PCMU;
                break;
            case codec_id::av1:
            case codec_id::opus:
                return false;
        }

        const auto media = rtsp_muxer_add_media(muxer_, payload, codec, track.codec_config.data(), static_cast<int>(track.codec_config.size()));
        if (media < 0)
        {
            return false;
        }
        tracks_.emplace(track.id,
                        track_state{
                            .kind = track.kind,
                            .codec = track.codec,
                            .config_version = track.config_version,
                            .media_id = media,
                        });
    }
    return true;
}

bool gb28181_output_media::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return true;
    }
    if (tracks->tracks.size() != tracks_.size())
    {
        return false;
    }

    bool video_changed = false;
    for (const auto& track : tracks->tracks)
    {
        const auto state = tracks_.find(track.id);
        if (state == tracks_.end() || state->second.codec != track.codec)
        {
            return false;
        }
        if (track.kind == media_kind::video && state->second.config_version != track.config_version)
        {
            video_changed = true;
        }
        state->second.config_version = track.config_version;
    }

    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
    return true;
}

int gb28181_output_media::on_muxer_packet(const void* data, int bytes)
{
    if (closed_ || data == nullptr || bytes <= 0 || !on_packet_)
    {
        return -1;
    }

    const auto* begin = static_cast<const std::uint8_t*>(data);
    on_packet_(std::vector<std::uint8_t>(begin, begin + bytes));
    return 0;
}

}    // namespace media_server
