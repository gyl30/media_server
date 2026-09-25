#ifndef MEDIA_GB28181_GB28181_RTP_SENDER_H
#define MEDIA_GB28181_GB28181_RTP_SENDER_H

#include <map>
#include <array>
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>

#include "media/ps/mpeg_ps_output.h"
#include "media/core/media_stream.h"

namespace media_server
{
class worker_context;

class gb28181_rtp_sender final : public media_reader_t<mpeg_ps_frame>, public std::enable_shared_from_this<gb28181_rtp_sender>
{
   public:
    using packet_handler = std::function<void(std::vector<std::uint8_t>)>;
    using end_handler = std::function<void()>;
    using failure_handler = std::function<void()>;

    gb28181_rtp_sender(worker_context& worker,
                       std::shared_ptr<media_stream> stream,
                       std::uint8_t payload_type,
                       std::uint32_t ssrc,
                       packet_handler on_packet,
                       end_handler on_end,
                       failure_handler on_failure = {});

   public:
    [[nodiscard]] static bool supported_tracks(const std::vector<media_track>& tracks);

   public:
    [[nodiscard]] bool startup();
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch_t<mpeg_ps_frame> batch) override;
    void on_end() override;

   private:
    struct track_state
    {
        media_kind kind{};
        std::uint64_t config_version{};
    };

   private:
    static void* allocate_packet(void* param, int bytes);
    static void free_packet(void* param, void* packet);
    static int packet_callback(void* param, const void* data, int bytes, std::uint32_t timestamp, int flags);

    [[nodiscard]] bool create_packetizer();
    void apply_tracks(const media_track_snapshot_ptr& tracks);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::uint8_t payload_type_{};
    std::uint32_t ssrc_{};
    packet_handler packet_handler_;
    end_handler end_handler_;
    failure_handler failure_handler_;
    std::shared_ptr<mpeg_ps_output> ps_output_;
    void* packetizer_{};
    std::array<std::uint8_t, 2048> packet_buffer_{};
    std::uint32_t timestamp_base_{};
    std::optional<std::uint32_t> first_media_timestamp_;
    std::map<track_id, track_state> track_states_;
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool waiting_for_key_frame_{true};
    std::atomic_bool shutdown_requested_{};
};

}    // namespace media_server

#endif
