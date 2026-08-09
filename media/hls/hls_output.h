#ifndef MEDIA_HLS_OUTPUT_H
#define MEDIA_HLS_OUTPUT_H

#include "media/core/media_sink.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> segment(std::uint64_t sequence) const;
    [[nodiscard]] std::size_t segment_count() const noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> ended_at() const noexcept;

   private:
    static void* ts_alloc(void* param, std::size_t bytes);
    static void ts_free(void* param, void* packet);
    static int ts_write(void* param, const void* packet, std::size_t bytes);

    void recreate_muxer();
    void finish_segment(std::int64_t end_pts_ns);
    [[nodiscard]] int add_track_to_muxer(const media_track& track);

    double target_duration_seconds_{};
    std::size_t window_size_{};
    std::map<track_id, media_track> tracks_;
    std::map<track_id, int> stream_ids_;
    std::deque<hls_segment> segments_;
    std::vector<std::uint8_t> current_segment_;
    void* muxer_{};
    std::uint64_t next_sequence_{};
    std::optional<std::int64_t> segment_start_pts_ns_;
    std::int64_t last_pts_ns_{};
    std::optional<std::chrono::steady_clock::time_point> ended_at_;
    bool has_video_{};
    bool waiting_for_key_frame_{};
};

}    // namespace media_server

#endif
