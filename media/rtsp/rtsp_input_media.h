#ifndef MEDIA_RTSP_RTSP_INPUT_MEDIA_H
#define MEDIA_RTSP_RTSP_INPUT_MEDIA_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

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

struct rtsp_input_track_description
{
    std::string uri;
    media_track track;
    int clock_rate{};
    int payload_type{};
    std::string encoding;
    std::string fmtp;
};

class rtsp_input_media final
{
   public:
    rtsp_input_media(worker_context& worker, std::string stream_name, std::vector<rtsp_input_track_description> descriptions);
    ~rtsp_input_media();

    [[nodiscard]] bool startup(const std::string& rtcp_cname);
    [[nodiscard]] bool start_recording();
    [[nodiscard]] bool input_packet(std::size_t track_index, std::span<const std::uint8_t> data);
    [[nodiscard]] int generate_rtcp(std::size_t track_index, std::span<std::uint8_t> buffer);
    void shutdown();

    [[nodiscard]] const std::vector<rtsp_input_track_description>& descriptions() const noexcept;
    [[nodiscard]] const std::string& stream_name() const noexcept;
    [[nodiscard]] bool recording() const noexcept;

   private:
    static int packet_callback(void* param, avpacket_t* packet);
    int on_demuxed_packet(avpacket_t* packet);
    bool update_track_from_packet(const avpacket_t& packet);

    worker_context& worker_;
    std::string stream_name_;
    std::vector<rtsp_input_track_description> descriptions_;
    std::vector<rtsp_demuxer_t*> demuxers_;
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    std::uint64_t rtcp_sync_ntp_{};
    std::int64_t rtcp_sync_pts_{};
    bool recording_{};
    bool rtcp_synchronized_{};
    bool fatal_codec_change_{};
    bool closed_{};
};

}    // namespace media_server

#endif
