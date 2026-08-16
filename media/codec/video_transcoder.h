#ifndef MEDIA_CODEC_VIDEO_TRANSCODER_H
#define MEDIA_CODEC_VIDEO_TRANSCODER_H

#include "media/core/media_types.h"

#include <memory>
#include <vector>

namespace media_server
{

struct video_transcoder_config
{
    codec_id input_codec{};
    codec_id output_codec{};
    std::vector<std::uint8_t> input_codec_config;
};

class video_transcoder final
{
   public:
    video_transcoder();
    ~video_transcoder();

    bool startup(const video_transcoder_config& config);
    void shutdown();
    bool transcode(const media_frame& input, std::vector<media_frame>& output);
    bool flush(std::vector<media_frame>& output);

   private:
    struct state;

    bool receive_decoded(std::vector<media_frame>& output, bool draining);
    bool startup_encoder();
    bool encode_decoded(std::vector<media_frame>& output);
    bool receive_encoded(std::vector<media_frame>& output, bool draining);

    std::unique_ptr<state> state_;
};

}    // namespace media_server

#endif
