#ifndef MEDIA_CODEC_OUTPUT_VIDEO_CONFIG_H
#define MEDIA_CODEC_OUTPUT_VIDEO_CONFIG_H

namespace media_server
{

enum class output_video_codec
{
    passthrough,
    av1,
};

struct output_video_config
{
    output_video_codec codec{output_video_codec::passthrough};
};

}    // namespace media_server

#endif
