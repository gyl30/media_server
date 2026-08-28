#ifndef MEDIA_GB28181_GB28181_INPUT_MEDIA_H
#define MEDIA_GB28181_GB28181_INPUT_MEDIA_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/any_io_executor.hpp>

#include "media/core/stream_registry.h"

extern "C"
{
#include "avpkt2bs.h"
}

struct avpacket_t;
struct rtsp_demuxer_t;

namespace media_server
{

enum class gb28181_rtp_input_result
{
    ignored,
    accepted,
    fatal,
};

class gb28181_input_media final
{
   public:
    gb28181_input_media(boost::asio::any_io_executor executor,
                        std::string stream_name,
                        std::uint8_t payload_type,
                        std::uint32_t expected_ssrc);
    ~gb28181_input_media();

    [[nodiscard]] bool startup();
    [[nodiscard]] gb28181_rtp_input_result input_rtp(std::span<const std::uint8_t> data);
    [[nodiscard]] int input_rtcp(std::span<const std::uint8_t> data);
    [[nodiscard]] int generate_rtcp(std::span<std::uint8_t> buffer);
    void shutdown();

    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    struct ps_topology
    {
        std::optional<codec_id> video;
        std::optional<codec_id> audio;
        bool invalid{};
    };

    static int packet_callback(void* param, avpacket_t* packet);
    static void stream_callback(void* param, int stream, int codecid, const void* extra, int bytes, int finish);

    int on_demuxed_packet(avpacket_t* packet);
    void on_stream(int codecid, bool finish);
    void apply_topology();
    bool update_track_from_packet(const avpacket_t& packet);
    bool try_start_recording();

    boost::asio::any_io_executor executor_;
    std::string stream_name_;
    std::uint8_t payload_type_{};
    std::uint32_t expected_ssrc_{};
    rtsp_demuxer_t* demuxer_{};
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    ps_topology pending_topology_;
    std::optional<codec_id> video_codec_;
    std::optional<codec_id> audio_codec_;
    std::optional<media_track> video_track_;
    std::optional<media_track> audio_track_;
    bool collecting_topology_{};
    bool topology_known_{};
    bool recording_{};
    bool fatal_codec_change_{};
    bool closed_{};
};

}    // namespace media_server

#endif
