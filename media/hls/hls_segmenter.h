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

class media_stream;

class hls_segmenter final : public media_sink, public std::enable_shared_from_this<hls_segmenter>
{
   public:
    bool startup(const std::shared_ptr<media_stream>& source);
    void shutdown();
    [[nodiscard]] worker_context& worker() noexcept override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    [[nodiscard]] std::string playlist(std::string_view base_path, std::string_view query = {}) const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> segment_buffer(std::uint64_t sequence) const;
    [[nodiscard]] bool has_segments() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> ended_at() const;

   private:
    struct track_state
    {
        media_track track;
        int stream_id{};
    };

    struct segment
    {
        std::uint64_t sequence{};
        double duration{};
        std::shared_ptr<const std::vector<std::uint8_t>> data;
    };

    static void* ts_alloc(void* param, std::size_t bytes);
    static void ts_free(void* param, void* packet);
    static int ts_write(void* param, const void* packet, std::size_t bytes);
    [[nodiscard]] bool recreate_muxer(std::map<track_id, track_state>& tracks);
    void discard_segment();
    void finish_segment(std::int64_t end_pts_ns);
    [[nodiscard]] static int add_track_to_muxer(void* muxer, const media_track& track);
    void finish();

   private:
    mutable std::mutex mutex_;
    std::map<track_id, track_state> tracks_;
    std::deque<segment> segments_;
    std::vector<std::uint8_t> current_segment_;
    void* muxer_{};
    std::uint64_t next_sequence_{};
    std::optional<std::int64_t> segment_start_pts_ns_;
    std::int64_t segment_max_pts_ns_{};
    std::optional<std::chrono::steady_clock::time_point> ended_at_;
    bool waiting_for_key_frame_{};
    bool has_video_{};
    std::shared_ptr<media_stream> source_;

};

}    // namespace media_server

#endif
