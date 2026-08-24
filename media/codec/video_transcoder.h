#ifndef MEDIA_CODEC_VIDEO_TRANSCODER_H
#define MEDIA_CODEC_VIDEO_TRANSCODER_H

#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

#include "media/core/media_types.h"

namespace media_server
{

struct av1_encoding_parameters
{
    std::uint8_t profile{};
    std::uint8_t level_idx{};
    std::uint8_t tier{};
};

struct video_transcoder_config
{
    codec_id input_codec{};
    codec_id output_codec{};
    std::vector<std::uint8_t> input_codec_config;
    std::optional<av1_encoding_parameters> av1{};
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
