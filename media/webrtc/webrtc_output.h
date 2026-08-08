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

// 第一阶段仅负责 WebRTC 媒体侧的 H.264 RTP 打包。
// ICE/DTLS/SRTP 由后续 transport 层接入，不进入 media_stream。
class webrtc_output final : public media_sink
{
   public:
    using rtp_handler = std::function<void(std::span<const std::uint8_t>)>;

    explicit webrtc_output(rtp_handler handler);
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
    };

    rtp_handler handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> tracks_;
    std::size_t packet_count_{};
};

}    // namespace media_server

#endif
