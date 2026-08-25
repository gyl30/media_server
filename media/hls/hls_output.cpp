#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/hls/hls_output.h"
#include "media/codec/codec_utils.h"

extern "C"
{
#include "aom-av1.h"
#include "mpeg-ts.h"
#include "mov-format.h"
#include "mpeg-proto.h"
#include "fmp4-writer.h"
}

namespace media_server
{

hls_output::hls_output(hls_config config)
    : video_config_(config.video), target_duration_seconds_(config.target_duration_seconds), window_size_(config.window_size)
{
    if (video_config_.codec == output_video_codec::passthrough)
    {
        recreate_muxer();
    }
}

hls_output::~hls_output()
{
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
    }
    if (fmp4_ != nullptr)
    {
        fmp4_writer_destroy(fmp4_);
    }
}

void hls_output::on_track(const media_track& track)
{
    std::scoped_lock lock(mutex_);
    const auto existing = tracks_.find(track.id);
    const bool reconfigured = existing != tracks_.end() && existing->second.config_version != track.config_version;
    tracks_.insert_or_assign(track.id, track);

    if (video_config_.codec == output_video_codec::av1)
    {
        if (reconfigured)
        {
            if (fmp4_ != nullptr && segment_start_pts_ns_)
            {
                finish_fmp4_segment(segment_max_pts_ns_);
            }
            reset_fmp4(true, track.kind == media_kind::video);
        }
        if (track.kind == media_kind::video)
        {
            startup_video_transcoder(track);
        }
        segment_start_pts_ns_.reset();
        segment_max_pts_ns_ = 0;
        waiting_for_key_frame_ = true;
        return;
    }

    if (!current_segment_.empty())
    {
        finish_segment(segment_max_pts_ns_);
    }
    recreate_muxer();
    segment_start_pts_ns_.reset();
    segment_max_pts_ns_ = 0;
    waiting_for_key_frame_ = true;
}

void hls_output::on_frame(const media_frame& frame)
{
    std::scoped_lock lock(mutex_);
    if (ended_at_.has_value() || !frame.payload)
    {
        return;
    }

    const auto track_iterator = tracks_.find(frame.track);
    if (track_iterator == tracks_.end())
    {
        return;
    }
    const auto& track = track_iterator->second;

    if (waiting_for_key_frame_)
    {
        if (track.kind != media_kind::video || !frame.key_frame)
        {
            return;
        }
        waiting_for_key_frame_ = false;
    }

    if (video_config_.codec == output_video_codec::av1)
    {
        if (track.kind == media_kind::video)
        {
            input_av1(frame);
        }
        else if (track.codec == codec_id::aac)
        {
            input_fmp4_audio(frame, track);
        }
        return;
    }

    if (muxer_ == nullptr)
    {
        return;
    }
    if (!segment_start_pts_ns_)
    {
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }

    const auto elapsed_ns = frame.pts_ns - *segment_start_pts_ns_;
    const auto target_ns = static_cast<std::int64_t>(target_duration_seconds_ * 1'000'000'000.0);
    const bool segment_boundary = track.kind == media_kind::video && frame.key_frame && elapsed_ns >= target_ns;
    if (segment_boundary && !current_segment_.empty())
    {
        finish_segment(frame.pts_ns);
        recreate_muxer();
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }

    const auto stream_iterator = stream_ids_.find(frame.track);
    if (stream_iterator == stream_ids_.end())
    {
        return;
    }

    const auto flags = frame.key_frame ? 1 : 0;
    const auto result = mpeg_ts_write(
        muxer_, stream_iterator->second, flags, ns_to_90khz(frame.pts_ns), ns_to_90khz(frame.dts_ns), frame.payload->data(), frame.payload->size());

    if (result != 0)
    {
        spdlog::error("hls ts write failed track {} result {}", frame.track, result);
    }
    segment_max_pts_ns_ = std::max(segment_max_pts_ns_, frame.pts_ns);
}

void hls_output::on_end()
{
    std::scoped_lock lock(mutex_);
    if (ended_at_.has_value())
    {
        return;
    }
    if (video_config_.codec == output_video_codec::av1)
    {
        if (fmp4_ != nullptr && segment_start_pts_ns_)
        {
            finish_fmp4_segment(segment_max_pts_ns_);
        }
        if (fmp4_ != nullptr)
        {
            fmp4_writer_destroy(fmp4_);
            fmp4_ = nullptr;
        }
        video_transcoder_.reset();
    }
    else
    {
        if (!current_segment_.empty())
        {
            finish_segment(segment_max_pts_ns_);
        }
        if (muxer_ != nullptr)
        {
            mpeg_ts_destroy(muxer_);
            muxer_ = nullptr;
            stream_ids_.clear();
        }
    }
    ended_at_ = std::chrono::steady_clock::now();
}

std::string hls_output::playlist(std::string_view base_path) const
{
    std::scoped_lock lock(mutex_);
    std::ostringstream output;
    output << "#EXTM3U\n";
    output << (video_config_.codec == output_video_codec::av1 ? "#EXT-X-VERSION:7\n" : "#EXT-X-VERSION:3\n");
    if (video_config_.codec == output_video_codec::av1)
    {
        output << "#EXT-X-MAP:URI=\"" << base_path << "/init.mp4?v=" << fmp4_init_revision_ << "\"\n";
    }
    double maximum_duration = target_duration_seconds_;
    for (const auto& item : segments_)
    {
        maximum_duration = std::max(maximum_duration, item.duration);
    }
    output << "#EXT-X-TARGETDURATION:" << std::max(1, static_cast<int>(std::ceil(maximum_duration))) << "\n";
    const auto first_sequence = segments_.empty() ? next_sequence_ : segments_.front().sequence;
    output << "#EXT-X-MEDIA-SEQUENCE:" << first_sequence << "\n";

    for (const auto& item : segments_)

    {
        output << "#EXTINF:" << std::fixed << std::setprecision(3) << item.duration << ",\n";
        output << base_path << '/' << item.sequence << (video_config_.codec == output_video_codec::av1 ? ".m4s\n" : ".ts\n");
    }

    if (ended_at_.has_value())

    {
        output << "#EXT-X-ENDLIST\n";
    }
    return output.str();
}

std::optional<std::vector<std::uint8_t>> hls_output::init_segment() const
{
    std::scoped_lock lock(mutex_);
    if (video_config_.codec != output_video_codec::av1 || init_segment_.empty())
    {
        return std::nullopt;
    }
    return init_segment_;
}

std::optional<std::vector<std::uint8_t>> hls_output::segment(std::uint64_t sequence) const
{
    std::scoped_lock lock(mutex_);
    const auto iterator = std::find_if(segments_.begin(), segments_.end(), [sequence](const hls_segment& item) { return item.sequence == sequence; });
    if (iterator == segments_.end())
    {
        return std::nullopt;
    }
    return iterator->data;
}

std::size_t hls_output::segment_count() const
{
    std::scoped_lock lock(mutex_);
    return segments_.size();
}

std::optional<std::chrono::steady_clock::time_point> hls_output::ended_at() const
{
    std::scoped_lock lock(mutex_);
    return ended_at_;
}

void* hls_output::ts_alloc(void*, std::size_t bytes) { return std::malloc(bytes); }

void hls_output::ts_free(void*, void* packet) { std::free(packet); }

int hls_output::ts_write(void* param, const void* packet, std::size_t bytes)
{
    auto* self = static_cast<hls_output*>(param);
    const auto* begin = static_cast<const std::uint8_t*>(packet);
    self->current_segment_.insert(self->current_segment_.end(), begin, begin + bytes);
    return 0;
}

int hls_output::mov_read(void* param, void* data, std::uint64_t bytes)
{
    auto* self = static_cast<hls_output*>(param);
    if (self->mov_target_ == nullptr || bytes > self->mov_target_->size() - std::min(self->mov_position_, self->mov_target_->size()))
    {
        return -1;
    }
    std::memcpy(data, self->mov_target_->data() + self->mov_position_, static_cast<std::size_t>(bytes));
    self->mov_position_ += static_cast<std::size_t>(bytes);
    return 0;
}

int hls_output::mov_write(void* param, const void* data, std::uint64_t bytes)
{
    auto* self = static_cast<hls_output*>(param);
    if (self->mov_target_ == nullptr || bytes > std::numeric_limits<std::size_t>::max() - self->mov_position_)
    {
        return -1;
    }
    const auto size = self->mov_position_ + static_cast<std::size_t>(bytes);
    if (self->mov_target_->size() < size)
    {
        self->mov_target_->resize(size);
    }
    std::memcpy(self->mov_target_->data() + self->mov_position_, data, static_cast<std::size_t>(bytes));
    self->mov_position_ = size;
    return 0;
}

int hls_output::mov_seek(void* param, std::int64_t offset)
{
    auto* self = static_cast<hls_output*>(param);
    if (self->mov_target_ == nullptr || self->mov_target_->size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return -1;
    }
    const auto size = static_cast<std::int64_t>(self->mov_target_->size());
    const auto position = offset >= 0 ? offset : size + offset;
    if (position < 0 || static_cast<std::uint64_t>(position) > std::numeric_limits<std::size_t>::max())
    {
        return -1;
    }
    self->mov_position_ = static_cast<std::size_t>(position);
    return 0;
}

std::int64_t hls_output::mov_tell(void* param)
{
    const auto* self = static_cast<hls_output*>(param);
    return self->mov_position_ <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ? static_cast<std::int64_t>(self->mov_position_)
                                                                                                     : -1;
}

void hls_output::reset_fmp4(bool clear_segments, bool clear_video_config)
{
    if (fmp4_ != nullptr)
    {
        fmp4_writer_destroy(fmp4_);
        fmp4_ = nullptr;
    }
    fmp4_video_track_ = -1;
    fmp4_audio_track_ = -1;
    fmp4_audio_track_id_ = 0;
    init_segment_.clear();
    current_segment_.clear();
    mov_target_ = nullptr;
    mov_position_ = 0;
    segment_start_pts_ns_.reset();
    segment_max_pts_ns_ = 0;
    if (clear_segments)
    {
        segments_.clear();
    }
    if (clear_video_config)
    {
        fmp4_video_config_.clear();
        fmp4_video_width_ = 0;
        fmp4_video_height_ = 0;
    }
}

void hls_output::startup_video_transcoder(const media_track& track)
{
    video_transcoder_.reset();
    video_track_id_ = 0;
    if (track.codec != codec_id::h264 && track.codec != codec_id::h265)
    {
        return;
    }
    auto transcoder = std::make_unique<video_transcoder>();
    if (!transcoder->startup(video_transcoder_config{
            .input_codec = track.codec,
            .output_codec = codec_id::av1,
            .input_codec_config = track.codec_config,
        }))
    {
        spdlog::error("hls av1 transcoder startup failed track {}", track.id);
        return;
    }
    video_track_id_ = track.id;
    video_transcoder_ = std::move(transcoder);
}

bool hls_output::ensure_fmp4(const media_frame& frame)
{
    if (fmp4_ != nullptr)
    {
        return true;
    }
    if (!frame.payload)
    {
        return false;
    }

    if (fmp4_video_config_.empty())
    {
        aom_av1_t av1{};
        if (aom_av1_codec_configuration_record_init(&av1, frame.payload->data(), frame.payload->size()) != 0 || av1.width == 0 || av1.height == 0)
        {
            spdlog::error("hls av1 configuration parse failed");
            return false;
        }
        fmp4_video_config_.resize(static_cast<std::size_t>(av1.bytes) + 4U);
        const auto bytes = aom_av1_codec_configuration_record_save(&av1, fmp4_video_config_.data(), fmp4_video_config_.size());
        if (bytes <= 0)
        {
            fmp4_video_config_.clear();
            spdlog::error("hls av1 configuration save failed");
            return false;
        }
        fmp4_video_config_.resize(static_cast<std::size_t>(bytes));
        fmp4_video_width_ = static_cast<int>(av1.width);
        fmp4_video_height_ = static_cast<int>(av1.height);
    }

    const mov_buffer_t buffer{
        .read = &hls_output::mov_read,
        .write = &hls_output::mov_write,
        .seek = &hls_output::mov_seek,
        .tell = &hls_output::mov_tell,
    };
    init_segment_.clear();
    mov_target_ = &init_segment_;
    mov_position_ = 0;
    fmp4_ = fmp4_writer_create(&buffer, this, MOV_FLAG_SEGMENT);
    if (fmp4_ == nullptr)
    {
        mov_target_ = nullptr;
        return false;
    }
    fmp4_video_track_ =
        fmp4_writer_add_video(fmp4_, MOV_OBJECT_AV1, fmp4_video_width_, fmp4_video_height_, fmp4_video_config_.data(), fmp4_video_config_.size());
    if (fmp4_video_track_ < 0)
    {
        reset_fmp4(false, false);
        return false;
    }

    for (const auto& [id, track] : tracks_)
    {
        if (track.kind != media_kind::audio || track.codec != codec_id::aac)
        {
            continue;
        }
        if (track.codec_config.empty() || track.clock_rate == 0 || track.channel_count == 0)
        {
            reset_fmp4(false, false);
            return false;
        }
        fmp4_audio_track_ = fmp4_writer_add_audio(
            fmp4_, MOV_OBJECT_AAC, track.channel_count, 16, static_cast<int>(track.clock_rate), track.codec_config.data(), track.codec_config.size());
        if (fmp4_audio_track_ < 0)
        {
            reset_fmp4(false, false);
            return false;
        }
        fmp4_audio_track_id_ = id;
        break;
    }

    if (fmp4_writer_init_segment(fmp4_) != 0 || init_segment_.empty())
    {
        reset_fmp4(false, false);
        return false;
    }
    fmp4_init_revision_ = next_sequence_;
    current_segment_.clear();
    mov_target_ = &current_segment_;
    mov_position_ = 0;
    return true;
}

void hls_output::input_av1(const media_frame& frame)
{
    if (!video_transcoder_ || frame.track != video_track_id_)
    {
        return;
    }
    std::vector<media_frame> output;
    if (!video_transcoder_->transcode(frame, output))
    {
        spdlog::error("hls av1 transcode failed track {}", frame.track);
        return;
    }
    for (const auto& encoded : output)
    {
        write_av1_frame(encoded);
    }
}

void hls_output::write_av1_frame(const media_frame& frame)
{
    if (!frame.payload || !ensure_fmp4(frame))
    {
        return;
    }
    if (!segment_start_pts_ns_)
    {
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }
    const auto elapsed_ns = frame.pts_ns - *segment_start_pts_ns_;
    const auto target_ns = static_cast<std::int64_t>(target_duration_seconds_ * 1'000'000'000.0);
    if (frame.key_frame && elapsed_ns >= target_ns)
    {
        finish_fmp4_segment(frame.pts_ns);
        segment_start_pts_ns_ = frame.pts_ns;
        segment_max_pts_ns_ = frame.pts_ns;
    }
    const auto flags = MOV_AV_FLAG_SEGMENT_DISABLE | (frame.key_frame ? MOV_AV_FLAG_KEYFREAME : 0);
    const auto result = fmp4_writer_write(fmp4_,
                                          fmp4_video_track_,
                                          frame.payload->data(),
                                          frame.payload->size(),
                                          ns_to_milliseconds(frame.pts_ns),
                                          ns_to_milliseconds(frame.dts_ns),
                                          flags);
    if (result != 0)
    {
        spdlog::error("hls av1 write failed result {}", result);
        return;
    }
    segment_max_pts_ns_ = std::max(segment_max_pts_ns_, frame.pts_ns);
}

void hls_output::input_fmp4_audio(const media_frame& frame, const media_track& track)
{
    if (fmp4_ == nullptr || fmp4_audio_track_ < 0 || frame.track != fmp4_audio_track_id_ || !frame.payload)
    {
        return;
    }
    const auto& data = *frame.payload;
    if (data.size() < 7U || data[0] != 0xffU || (data[1] & 0xf6U) != 0xf0U)
    {
        spdlog::error("hls fmp4 invalid aac adts track {}", track.id);
        return;
    }
    const std::size_t header_size = (data[1] & 0x01U) != 0U ? 7U : 9U;
    if (data.size() <= header_size)
    {
        return;
    }
    const auto result = fmp4_writer_write(fmp4_,
                                          fmp4_audio_track_,
                                          data.data() + header_size,
                                          data.size() - header_size,
                                          ns_to_milliseconds(frame.pts_ns),
                                          ns_to_milliseconds(frame.dts_ns),
                                          MOV_AV_FLAG_SEGMENT_DISABLE);
    if (result != 0)
    {
        spdlog::error("hls fmp4 aac write failed track {} result {}", track.id, result);
        return;
    }
    segment_max_pts_ns_ = std::max(segment_max_pts_ns_, frame.pts_ns);
}

void hls_output::finish_fmp4_segment(std::int64_t end_pts_ns)
{
    if (fmp4_ == nullptr || !segment_start_pts_ns_)
    {
        return;
    }
    if (fmp4_writer_save_segment(fmp4_) != 0)
    {
        spdlog::error("hls fmp4 segment save failed");
        return;
    }
    if (!current_segment_.empty())
    {
        double duration = static_cast<double>(end_pts_ns - *segment_start_pts_ns_) / 1'000'000'000.0;
        if (duration <= 0.0)
        {
            duration = target_duration_seconds_;
        }
        segments_.push_back(hls_segment{
            .sequence = next_sequence_++,
            .duration = duration,
            .data = std::move(current_segment_),
        });
        current_segment_.clear();
        while (segments_.size() > window_size_)
        {
            segments_.pop_front();
        }
    }
    mov_target_ = &current_segment_;
    mov_position_ = 0;
}

void hls_output::recreate_muxer()
{
    if (muxer_ != nullptr)
    {
        mpeg_ts_destroy(muxer_);
    }

    const mpeg_ts_func_t functions{
        .alloc = &hls_output::ts_alloc,
        .free = &hls_output::ts_free,
        .write = &hls_output::ts_write,
    };
    muxer_ = mpeg_ts_create(&functions, this);
    stream_ids_.clear();

    for (const auto& [id, track] : tracks_)

    {
        const auto stream_id = add_track_to_muxer(track);
        if (stream_id > 0)
        {
            stream_ids_.insert_or_assign(id, stream_id);
        }
    }
}

void hls_output::finish_segment(std::int64_t end_pts_ns)
{
    if (current_segment_.empty())
    {
        return;
    }

    double duration = target_duration_seconds_;
    if (segment_start_pts_ns_ && end_pts_ns >= *segment_start_pts_ns_)
    {
        duration = static_cast<double>(end_pts_ns - *segment_start_pts_ns_) / 1'000'000'000.0;
    }
    if (duration <= 0.0)
    {
        duration = target_duration_seconds_;
    }

    segments_.push_back(hls_segment{
        .sequence = next_sequence_++,
        .duration = duration,
        .data = std::move(current_segment_),
    });
    current_segment_.clear();

    while (segments_.size() > window_size_)

    {
        segments_.pop_front();
    }
}

int hls_output::add_track_to_muxer(const media_track& track)
{
    switch (track.codec)
    {
        case codec_id::h264:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_H264, nullptr, 0);
        case codec_id::h265:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_H265, nullptr, 0);
        case codec_id::aac:
            return mpeg_ts_add_stream(muxer_, PSI_STREAM_AAC, nullptr, 0);
        case codec_id::g711a:
            return track.clock_rate == 8'000 && track.channel_count == 1 && track.codec_config.empty()
                       ? mpeg_ts_add_stream(muxer_, PSI_STREAM_AUDIO_G711A, nullptr, 0)
                       : -1;
        case codec_id::g711u:
            return track.clock_rate == 8'000 && track.channel_count == 1 && track.codec_config.empty()
                       ? mpeg_ts_add_stream(muxer_, PSI_STREAM_AUDIO_G711U, nullptr, 0)
                       : -1;
        case codec_id::av1:
        case codec_id::opus:
            return -1;
    }
    return -1;
}

}    // namespace media_server
