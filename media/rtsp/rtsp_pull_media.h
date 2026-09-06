#ifndef MEDIA_RTSP_RTSP_PULL_MEDIA_H
#define MEDIA_RTSP_RTSP_PULL_MEDIA_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    std::size_t media{};
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
    rtsp_pull_media(worker_context& worker, std::string stream_name, std::vector<rtsp_pull_track_description> descriptions);
    ~rtsp_pull_media();

    [[nodiscard]] bool startup();
    [[nodiscard]] bool input_packet(std::uint8_t channel, std::span<const std::uint8_t> data);
    int set_rtp_info(std::size_t media, std::uint16_t sequence, std::uint32_t timestamp);
    int generate_rtcp(std::size_t media, std::span<std::uint8_t> buffer);
    [[nodiscard]] bool tracks_initialized() const noexcept;
    void shutdown();

   private:
    static int packet_callback(void* param, avpacket_t* packet);
    int on_demuxed_packet(avpacket_t* packet);
    [[nodiscard]] bool update_track_from_packet(const avpacket_t& packet);
    [[nodiscard]] bool try_initialize_tracks();

    worker_context& worker_;
    std::string stream_name_;
    std::vector<rtsp_pull_track_description> descriptions_;
    std::vector<rtsp_demuxer_t*> demuxers_;
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    std::optional<media_track> initial_video_track_;
    std::optional<media_track> initial_audio_track_;
    bool expected_audio_{};
    bool tracks_initialized_{};
    bool fatal_{};
    bool closed_{};
};

}    // namespace media_server

#endif
