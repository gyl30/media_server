#ifndef MEDIA_WEBRTC_OUTPUT_H
#define MEDIA_WEBRTC_OUTPUT_H

#include "media/codec/audio_transcoder.h"
#include "media/core/media_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct rtsp_muxer_t;

namespace media_server
{

struct webrtc_output_config
{
    codec_id video_codec{codec_id::h264};
    int video_payload_type{-1};
    int opus_payload_type{-1};
    int opus_channel_count{1};
    int opus_bitrate{-1};
    int opus_max_playback_rate{48'000};
    std::string video_mid{};
    std::string audio_mid{};
    int video_mid_extension_id{-1};
    int audio_mid_extension_id{-1};
    std::string rtcp_cname;
};

class webrtc_output final
{
   public:
    using packet_handler = std::function<void(std::span<const std::uint8_t>)>;

    webrtc_output(webrtc_output_config config, packet_handler rtp_handler, packet_handler rtcp_handler = {});
    ~webrtc_output();

    void on_track(const media_track& track);
    void on_frame(const media_frame& frame);

    [[nodiscard]] bool valid() const noexcept;

   private:
    static int on_packet(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

    struct track_state
    {
        codec_id codec{};
        std::unique_ptr<audio_transcoder> transcoder;
        int media_id{-1};
        int payload_id{-1};
        bool waiting_key_frame{};
    };

    bool add_h264_track(const media_track& track);
    bool add_h265_track(const media_track& track);
    bool add_aac_track(const media_track& track);
    bool configure_rtcp(int payload_id);
    void emit_rtcp(int payload_id);
    void input_video(track_state& state, const media_frame& frame);
    void input_aac(track_state& state, const media_frame& frame);

    webrtc_output_config config_;
    packet_handler rtp_handler_;
    packet_handler rtcp_handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> tracks_;
};

}    // namespace media_server

#endif
