#ifndef MEDIA_FLV_MUXER_H
#define MEDIA_FLV_MUXER_H

#include <map>
#include <span>
#include <memory>
#include <cstdint>
#include <functional>

#include "media/core/media_types.h"
#include "media/codec/video_transcoder.h"
#include "media/codec/video_transcode_config.h"

struct flv_muxer_t;

namespace media_server
{

class flv_muxer final
{
   public:
    using packet_handler = std::function<void(int, std::span<const std::uint8_t>, std::uint32_t)>;

    explicit flv_muxer(packet_handler handler, video_transcode_config video = {});
    ~flv_muxer();

    void shutdown();
    void on_track(const media_track& track);
    void on_frame(const media_frame& frame);

   private:
    static int on_packet(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp);

    void prime_video_config(const media_track& track, std::uint32_t timestamp);
    void startup_video_transcoder(const media_track& track);
    void input_av1(const media_frame& frame);

    packet_handler packet_handler_;
    video_transcode_config video_config_;
    flv_muxer_t* muxer_{};
    std::map<track_id, media_track> tracks_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    bool video_config_pending_{};
};

}    // namespace media_server

#endif
