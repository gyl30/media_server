#ifndef MEDIA_WEBRTC_OUTPUT_H
#define MEDIA_WEBRTC_OUTPUT_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>

#include "media/core/media_types.h"
#include "media/codec/audio_transcoder.h"
#include "media/codec/video_transcoder.h"

struct rtsp_muxer_t;

namespace media_server
{

struct webrtc_output_config
{
    codec_id video_codec{codec_id::h264};
    codec_id audio_codec{codec_id::aac};
    int video_payload_type{-1};
    int audio_payload_type{-1};
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
    void shutdown();

    [[nodiscard]] bool valid() const noexcept;

   private:
    static int on_packet(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

    struct track_state
    {
        codec_id codec{};
        std::unique_ptr<audio_transcoder> transcoder;
        std::unique_ptr<video_transcoder> video_transcoder_;
        int media_id{-1};
        int payload_id{-1};
        std::size_t rtp_extension_bytes{};
        bool waiting_key_frame{};
    };

    bool add_h264_track(const media_track& track);
    bool add_h265_track(const media_track& track);
    bool add_av1_track(const media_track& track);
    bool add_audio_track(const media_track& track);
    bool configure_rtcp(int payload_id);
    void remove_track(track_id id);
    void emit_rtcp(int payload_id);
    void input_video(track_state& state, const media_frame& frame);
    void input_audio(track_state& state, const media_frame& frame);

    webrtc_output_config config_;
    packet_handler rtp_handler_;
    packet_handler rtcp_handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> track_states_;
};

}    // namespace media_server

#endif
