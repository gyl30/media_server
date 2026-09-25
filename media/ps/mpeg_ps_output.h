#ifndef MEDIA_PS_MPEG_PS_OUTPUT_H
#define MEDIA_PS_MPEG_PS_OUTPUT_H

#include "media/core/media_history.h"
#include "media/core/media_sink.h"

struct ps_muxer_t;

namespace media_server
{

struct mpeg_ps_frame
{
    track_id track{};
    std::int64_t dts_ns{};
    std::int64_t pts_ns{};
    bool key_frame{};
    byte_buffer payload;
    std::uint32_t media_timestamp{};
};

class mpeg_ps_output final : public media_sink
{
   public:
    mpeg_ps_output(std::string name, worker_context& worker);

    [[nodiscard]] static bool supported_tracks(const std::vector<media_track>& tracks);
    [[nodiscard]] bool startup(const std::vector<media_track>& tracks);
    [[nodiscard]] std::shared_ptr<media_history<mpeg_ps_frame>> stream() const noexcept;

    void on_track(const media_track& track) override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;
    [[nodiscard]] bool failed() const noexcept;

   private:
    static void* allocate_packet(void* param, std::size_t bytes);
    static void free_packet(void* param, void* packet);
    static int write_packet(void* param, int stream, void* packet, std::size_t bytes);

    std::shared_ptr<media_history<mpeg_ps_frame>> output_;
    std::unique_ptr<ps_muxer_t, int (*)(ps_muxer_t*)> muxer_;
    std::map<track_id, std::pair<media_track, int>> tracks_;
    std::shared_ptr<std::vector<std::uint8_t>> packet_;
    bool waiting_for_key_frame_{true};
    std::atomic_bool failed_{};
};

}    // namespace media_server

#endif
