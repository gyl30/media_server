#ifndef MEDIA_WEBRTC_WHIP_MEDIA_RECEIVER_H
#define MEDIA_WEBRTC_WHIP_MEDIA_RECEIVER_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include "media/core/media_stream.h"
#include "media/codec/audio_transcoder.h"

extern "C"
{
#include "avpkt2bs.h"
}

struct avpacket_t;
struct rtsp_demuxer_t;

namespace media_server
{

class worker_context;

struct whip_media_receiver_config
{
    codec_id video_codec{codec_id::h264};
    int video_payload_type{-1};
    int audio_payload_type{-1};
    std::uint16_t audio_channel_count{2};
};

class whip_media_receiver final
{
   public:
    whip_media_receiver(worker_context& worker, std::string stream_name);
    ~whip_media_receiver();

   public:
    [[nodiscard]] bool startup(whip_media_receiver_config config);
    [[nodiscard]] bool input_rtp(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool input_rtcp(std::span<const std::uint8_t> packet);

   private:
    static int packet_callback(void* param, avpacket_t* packet);

   private:
    int on_demuxed_packet(avpacket_t* packet);
    bool apply_sender_report(rtsp_demuxer_t* demuxer);

   private:
    std::shared_ptr<media_stream> media_stream_;
    int video_payload_type_{};
    int audio_payload_type_{-1};
    rtsp_demuxer_t* video_demuxer_{};
    rtsp_demuxer_t* audio_demuxer_{};
    std::unique_ptr<audio_transcoder> audio_transcoder_;
    avpkt2bs_t bitstream_{};
    std::optional<media_track> audio_track_;
    std::optional<std::uint32_t> video_ssrc_;
    std::optional<std::uint32_t> audio_ssrc_;
    struct rtcp_sync
    {
        std::uint64_t ntp{};
        std::int64_t pts{};
    };
    std::optional<rtcp_sync> rtcp_sync_;
};

}    // namespace media_server

#endif
