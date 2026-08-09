#ifndef MEDIA_WEBRTC_OUTPUT_H
#define MEDIA_WEBRTC_OUTPUT_H

#include "media/core/media_sink.h"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

struct rtsp_muxer_t;

namespace media_server
{

struct webrtc_output_config
{
    int h264_payload_type{};
};

class webrtc_output final : public media_sink
{
   public:
    using rtp_handler = std::function<void(std::span<const std::uint8_t>)>;

    webrtc_output(webrtc_output_config config, rtp_handler handler);
    ~webrtc_output() override;

    void on_track(const media_track& track) override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    [[nodiscard]] std::size_t packet_count() const noexcept;

   private:
    static int on_packet(
        void* param,
        int pid,
        const void* data,
        int bytes,
        std::uint32_t timestamp,
        int flags);

    struct track_state
    {
        media_track track;
        int media_id{-1};
        bool waiting_key_frame{true};
    };

    webrtc_output_config config_;
    rtp_handler handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> tracks_;
    std::size_t packet_count_{};
};

}    // namespace media_server

#endif
