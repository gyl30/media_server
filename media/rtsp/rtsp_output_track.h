#ifndef MEDIA_RTSP_RTSP_OUTPUT_TRACK_H
#define MEDIA_RTSP_RTSP_OUTPUT_TRACK_H

#include "media/core/media_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace media_server
{

struct rtsp_output_track_description
{
    media_track track;
    int rtp_codec{-1};
    int frequency{};
    int payload_type{-1};
    std::string encoding;
    std::vector<std::uint8_t> extra;
};

}    // namespace media_server

#endif
