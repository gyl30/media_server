#ifndef MEDIA_FLV_MUXER_H
#define MEDIA_FLV_MUXER_H

#include <map>
#include <span>
#include <memory>
#include <cstdint>
#include <functional>

#include "media/core/media_types.h"

struct flv_muxer_t;

namespace media_server
{

class flv_muxer final
{
   public:
    using packet_handler = std::function<void(int, std::span<const std::uint8_t>, std::uint32_t)>;

    explicit flv_muxer(packet_handler handler);

   public:
    void shutdown();
    void on_track(const media_track& track);
    void on_frame(const media_frame& frame);

   private:
    static int on_packet(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp);

    void prime_video_config(const media_track& track, std::uint32_t timestamp);

   private:
    packet_handler packet_handler_;
    flv_muxer_t* muxer_{};
    std::map<track_id, media_track> tracks_;
};

}    // namespace media_server

#endif
