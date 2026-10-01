#ifndef MEDIA_WEBRTC_PACKETIZER_H
#define MEDIA_WEBRTC_PACKETIZER_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>

#include "media/core/media_types.h"

struct rtsp_muxer_t;

namespace media_server
{
struct webrtc_packetizer_config
{
    codec_id video_codec{codec_id::h264};
    codec_id audio_codec{codec_id::opus};
    int video_payload_type{-1};
    int audio_payload_type{-1};
    std::string video_mid{};
    std::string audio_mid{};
    int video_mid_extension_id{-1};
    int audio_mid_extension_id{-1};
    std::string rtcp_cname;
};

class webrtc_packetizer final
{
   public:
    using packet_handler = std::function<int(std::span<const std::uint8_t>)>;

    webrtc_packetizer(packet_handler rtp_handler, packet_handler rtcp_handler);
    ~webrtc_packetizer();

   public:
    bool startup(std::span<const media_track> tracks, const webrtc_packetizer_config& config);
    bool on_frame(const media_frame& frame);

   private:
    static int on_packet(void* param, int payload_index, const void* data, int bytes, std::uint32_t timestamp, int flags);

   private:
    struct track_state
    {
        codec_id codec{};
        int media_index{-1};
        int payload_index{-1};
        std::string mid;
        int mid_extension_id{};
    };

    [[nodiscard]] bool emit_rtcp(int payload_index);
    bool input_video(track_state& state, const media_frame& frame);
    bool input_audio(track_state& state, const media_frame& frame);

   private:
    packet_handler rtp_handler_;
    packet_handler rtcp_handler_;
    rtsp_muxer_t* muxer_{};
    std::map<track_id, track_state> track_states_;
};

}    // namespace media_server

#endif
