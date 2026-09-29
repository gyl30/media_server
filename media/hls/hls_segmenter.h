#ifndef MEDIA_HLS_SEGMENTER_H
#define MEDIA_HLS_SEGMENTER_H

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

namespace media_server
{

struct hls_config
{
    double target_duration_seconds{2.0};
    std::size_t window_size{6};
};

struct hls_segment
{
    std::uint64_t sequence{};
    double duration{};
    std::shared_ptr<const std::vector<std::uint8_t>> data;
};

class media_stream;

class hls_segmenter final : public media_sink, public std::enable_shared_from_this<hls_segmenter>
{
   public:
    explicit hls_segmenter(hls_config config = {});

   public:
    bool startup(const std::shared_ptr<media_stream>& source);
    void shutdown();
    [[nodiscard]] worker_context& worker() noexcept override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    [[nodiscard]] std::string playlist(std::string_view base_path, std::string_view query = {}) const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> segment_buffer(std::uint64_t sequence) const;
    [[nodiscard]] std::size_t segment_count() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> ended_at() const;

   private:
    static void* ts_alloc(void* param, std::size_t bytes);
    static void ts_free(void* param, void* packet);
    static int ts_write(void* param, const void* packet, std::size_t bytes);
    [[nodiscard]] bool recreate_muxer(const std::map<track_id, media_track>& tracks);
    void discard_segment();
    void finish_segment(std::int64_t end_pts_ns);
    [[nodiscard]] static int add_track_to_muxer(void* muxer, const media_track& track);
    void process_frame(const media_frame& frame);
    void finish();

   private:
    mutable std::mutex mutex_;
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
    std::shared_ptr<media_stream> source_;

};

}    // namespace media_server

#endif
