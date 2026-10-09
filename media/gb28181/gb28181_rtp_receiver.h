#ifndef MEDIA_GB28181_GB28181_RTP_RECEIVER_H
#define MEDIA_GB28181_GB28181_RTP_RECEIVER_H

#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include "media/core/media_types.h"
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

enum class gb28181_rtp_receive_result
{
    ignored,
    accepted,
    fatal,
};

class gb28181_rtp_receiver final
{
   public:
    gb28181_rtp_receiver(worker_context& worker, std::string stream_name, std::uint8_t payload_type, std::uint32_t expected_ssrc);

   public:
    [[nodiscard]] bool startup();
    [[nodiscard]] gb28181_rtp_receive_result receive_rtp(std::span<const std::uint8_t> data);
    [[nodiscard]] bool receive_rtcp(std::span<const std::uint8_t> data);
    [[nodiscard]] int generate_rtcp(std::span<std::uint8_t> buffer);
    void shutdown();
    // 设备可以在 200 OK 的 y= 中改用自己的 SSRC，信令据此更新过滤条件。
    void set_expected_ssrc(std::uint32_t ssrc) noexcept { expected_ssrc_ = ssrc; }

   public:
    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    struct ps_topology
    {
        std::optional<codec_id> video;
        std::optional<codec_id> audio;
        bool unsupported_audio{};
        bool invalid{};
    };

   private:
    static int packet_callback(void* param, avpacket_t* packet);
    static void stream_callback(void* param, int stream, int codecid, const void* extra, int bytes, int finish);

    int on_demuxed_packet(avpacket_t* packet);
    void on_stream(int codecid, bool finish);
    [[nodiscard]] bool apply_topology(const ps_topology& topology);
    [[nodiscard]] int update_track_from_packet(const avpacket_t& packet);

   private:
    worker_context& worker_;
    std::string stream_name_;
    std::uint8_t payload_type_{};
    std::uint32_t expected_ssrc_{};
    rtsp_demuxer_t* demuxer_{};
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    ps_topology pending_topology_;
    std::optional<ps_topology> announced_topology_;
    std::optional<codec_id> video_codec_;
    std::optional<codec_id> audio_codec_;
    std::optional<media_track> video_track_;
    std::optional<media_track> audio_track_;
    std::optional<int> video_stream_;
    std::optional<int> audio_stream_;
};

}    // namespace media_server

#endif
