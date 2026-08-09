#ifndef MEDIA_FLV_OUTPUT_MUXER_H
#define MEDIA_FLV_OUTPUT_MUXER_H

#include "media/core/media_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <span>

struct flv_muxer_t;

namespace media_server
{

class flv_output_muxer final
{
   public:
    using output_handler = std::function<void(int, std::span<const std::uint8_t>, std::uint32_t)>;

    explicit flv_output_muxer(output_handler handler);
    ~flv_output_muxer();


    void on_track(const media_track& track);
    void on_frame(const media_frame& frame);

   private:
    static int on_output(
        void* param,
        int type,
        const void* data,
        std::size_t bytes,
        std::uint32_t timestamp);

    void prime_video_config(const media_track& track);

    output_handler handler_;
    flv_muxer_t* muxer_{};
    std::map<track_id, media_track> tracks_;
};

}    // namespace media_server

#endif
