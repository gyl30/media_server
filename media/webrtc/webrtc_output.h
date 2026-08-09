#ifndef MEDIA_WEBRTC_OUTPUT_H
#define MEDIA_WEBRTC_OUTPUT_H

#include "media/codec/aac_opus_transcoder.h"
#include "media/core/media_sink.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

struct rtsp_muxer_t;

namespace media_server
{

struct webrtc_output_config
{
    int h264_payload_type{-1};
    int opus_payload_type{-1};
    int opus_channel_count{1};
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

    bool add_h264_track(const media_track& track);
    bool add_aac_track(const media_track& track);
    void input_h264(track_id id, const media_frame& frame);
    void input_aac(track_id id, const media_frame& frame);

    struct track_state
    {
        media_track track;
        std::unique_ptr<aac_opus_transcoder> transcoder;
        std::int64_t audio_pts_ns{};
        int media_id{-1};
        bool waiting_key_frame{};
        bool audio_pts_started{};
    };

    webrtc_output_config config_;
    rtp_handler handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> tracks_;
    std::size_t packet_count_{};
};

}    // namespace media_server

#endif
