#ifndef MEDIA_HLS_OUTPUT_H
#define MEDIA_HLS_OUTPUT_H

#include <map>
#include <deque>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "media/core/media_sink.h"
#include "media/codec/video_transcoder.h"
#include "media/codec/output_video_config.h"

struct fmp4_writer_t;

namespace media_server
{

struct hls_config
{
    double target_duration_seconds{2.0};
    std::size_t window_size{6};
    output_video_config video;
};

struct hls_segment
{
    std::uint64_t sequence{};
    double duration{};
    std::vector<std::uint8_t> data;
};

class hls_output final : public media_sink
{
   public:
    explicit hls_output(hls_config config = {});
    ~hls_output() override;

    void on_track(const media_track& track) override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    [[nodiscard]] std::string playlist(std::string_view base_path) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> init_segment() const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> segment(std::uint64_t sequence) const;
    [[nodiscard]] std::size_t segment_count() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> ended_at() const;

   private:
    static void* ts_alloc(void* param, std::size_t bytes);
    static void ts_free(void* param, void* packet);
    static int ts_write(void* param, const void* packet, std::size_t bytes);
    static int mov_read(void* param, void* data, std::uint64_t bytes);
    static int mov_write(void* param, const void* data, std::uint64_t bytes);
    static int mov_seek(void* param, std::int64_t offset);
    static std::int64_t mov_tell(void* param);

    void recreate_muxer();
    void finish_segment(std::int64_t end_pts_ns);
    [[nodiscard]] int add_track_to_muxer(const media_track& track);
    void reset_fmp4(bool clear_segments, bool clear_video_config);
    void startup_video_transcoder(const media_track& track);
    bool ensure_fmp4(const media_frame& frame);
    void input_av1(const media_frame& frame);
    void write_av1_frame(const media_frame& frame);
    void input_fmp4_audio(const media_frame& frame, const media_track& track);
    void finish_fmp4_segment(std::int64_t end_pts_ns);

    mutable std::mutex mutex_;
    output_video_config video_config_;
    double target_duration_seconds_{};
    std::size_t window_size_{};
    std::map<track_id, media_track> tracks_;
    std::map<track_id, int> stream_ids_;
    std::deque<hls_segment> segments_;
    std::vector<std::uint8_t> current_segment_;
    void* muxer_{};
    std::uint64_t next_sequence_{};
    std::optional<std::int64_t> segment_start_pts_ns_;
    std::int64_t segment_max_pts_ns_{};
    std::optional<std::chrono::steady_clock::time_point> ended_at_;
    bool waiting_for_key_frame_{};

    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    fmp4_writer_t* fmp4_{};
    int fmp4_video_track_{-1};
    int fmp4_audio_track_{-1};
    track_id fmp4_audio_track_id_{};
    std::vector<std::uint8_t> init_segment_;
    std::uint64_t fmp4_init_revision_{};
    std::vector<std::uint8_t> fmp4_video_config_;
    int fmp4_video_width_{};
    int fmp4_video_height_{};
    std::vector<std::uint8_t>* mov_target_{};
    std::size_t mov_position_{};
};

}    // namespace media_server

#endif
