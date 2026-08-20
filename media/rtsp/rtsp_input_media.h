#ifndef MEDIA_RTSP_RTSP_INPUT_MEDIA_H
#define MEDIA_RTSP_RTSP_INPUT_MEDIA_H

#include "media/core/stream_registry.h"

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C"
{
#include "avpkt2bs.h"
}

struct avpacket_t;
struct rtsp_demuxer_t;

namespace media_server
{

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
    rtsp_input_media(stream_registry& registry,
                     boost::asio::any_io_executor executor,
                     std::string stream_name,
                     std::string rtcp_cname,
                     std::vector<rtsp_input_track_description> descriptions);
    ~rtsp_input_media();

    [[nodiscard]] bool startup();
    [[nodiscard]] bool start_recording();
    [[nodiscard]] bool input(std::size_t track_index, std::span<const std::uint8_t> data);
    [[nodiscard]] int rtcp(std::size_t track_index, std::span<std::uint8_t> buffer);
    void shutdown();

    [[nodiscard]] const std::vector<rtsp_input_track_description>& descriptions() const noexcept;
    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    static int packet_callback(void* param, avpacket_t* packet);
    int on_packet(avpacket_t* packet);
    bool update_track_from_packet(const avpacket_t& packet);

    stream_registry& registry_;
    boost::asio::any_io_executor executor_;
    std::string stream_name_;
    std::string rtcp_cname_;
    std::vector<rtsp_input_track_description> descriptions_;
    std::vector<rtsp_demuxer_t*> demuxers_;
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    bool recording_{};
    bool fatal_codec_change_{};
    bool closed_{};
};

}    // namespace media_server

#endif
