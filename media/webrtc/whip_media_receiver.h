#ifndef MEDIA_WEBRTC_WHIP_MEDIA_RECEIVER_H
#define MEDIA_WEBRTC_WHIP_MEDIA_RECEIVER_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include "media/codec/audio_transcoder.h"
#include "media/core/media_stream.h"

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
    whip_media_receiver(worker_context& worker, std::string stream_name, whip_media_receiver_config config);
    ~whip_media_receiver();

    [[nodiscard]] bool startup();
    [[nodiscard]] bool input_rtp(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool input_rtcp(std::span<const std::uint8_t> packet);
    void shutdown();

   private:
    static int packet_callback(void* param, avpacket_t* packet);

    int on_demuxed_packet(avpacket_t* packet);
    bool update_video_track(const avpacket_t& packet);
    bool publish_stream();
    bool apply_sender_report(rtsp_demuxer_t* demuxer);

    worker_context& worker_;
    std::string stream_name_;
    whip_media_receiver_config config_;
    std::shared_ptr<media_stream> media_stream_;
    rtsp_demuxer_t* video_demuxer_{};
    rtsp_demuxer_t* audio_demuxer_{};
    audio_transcoder audio_transcoder_;
    avpkt2bs_t bitstream_{};
    std::optional<media_track> video_track_;
    std::optional<media_track> audio_track_;
    std::optional<std::uint32_t> video_ssrc_;
    std::optional<std::uint32_t> audio_ssrc_;
    std::uint64_t rtcp_sync_ntp_{};
    std::int64_t rtcp_sync_pts_{};
    bool published_{};
    bool rtcp_synchronized_{};
    bool closed_{};
};

}    // namespace media_server

#endif
