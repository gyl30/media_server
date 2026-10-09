#ifndef MEDIA_RTSP_RTSP_PULL_MEDIA_H
#define MEDIA_RTSP_RTSP_PULL_MEDIA_H

#include <span>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>

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

struct rtsp_pull_track_description
{
    media_kind kind{};
    int clock_rate{};
    int payload_type{};
    std::string encoding;
    std::string fmtp;
    std::optional<media_track> initial_track;
};

class rtsp_pull_media final
{
   public:
    // media_handler 在解复用出完整媒体帧时调用，供输入空闲计时使用。
    rtsp_pull_media(worker_context& worker, std::string media_stream_name, std::function<void()> media_handler);

   public:
    [[nodiscard]] bool startup(std::vector<rtsp_pull_track_description> descriptions);
    [[nodiscard]] bool input_packet(std::uint8_t channel, std::span<const std::uint8_t> data);
    int set_rtp_info(std::size_t media, std::uint16_t sequence, std::uint32_t timestamp);
    int generate_rtcp(std::size_t media, std::span<std::uint8_t> buffer);
    void shutdown();

   private:
    static int packet_callback(void* param, avpacket_t* packet);

   private:
    int on_demuxed_packet(avpacket_t* packet);
    [[nodiscard]] int update_track_from_packet(const avpacket_t& packet);
    [[nodiscard]] int register_stream_if_ready();

   private:
    worker_context& worker_;
    std::string media_stream_name_;
    std::function<void()> media_handler_;
    std::vector<rtsp_demuxer_t*> demuxers_;
    std::shared_ptr<media_stream> media_stream_;
    avpkt2bs_t bitstream_{};
    std::optional<media_track> initial_video_track_;
    std::optional<media_track> initial_audio_track_;
};

}    // namespace media_server

#endif
