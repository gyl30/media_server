#ifndef MEDIA_CODEC_VIDEO_TRANSCODE_CONFIG_H
#define MEDIA_CODEC_VIDEO_TRANSCODE_CONFIG_H

namespace media_server
{

enum class video_transcode_codec
{
    passthrough,
    av1,
};

struct video_transcode_config
{
    video_transcode_codec codec{video_transcode_codec::passthrough};
};

}    // namespace media_server

#endif
