#ifndef MEDIA_RTSP_RTSP_OUTPUT_TRACK_H
#define MEDIA_RTSP_RTSP_OUTPUT_TRACK_H

#include <string>
#include <vector>
#include <cstdint>

#include "media/core/media_types.h"

namespace media_server
{

[[nodiscard]] inline bool rtsp_output_track_supported(const media_track& track)
{
    return (track.kind == media_kind::video && (track.codec == codec_id::h264 || track.codec == codec_id::h265)) ||
           (track.kind == media_kind::audio && (track.codec == codec_id::aac ||
                                                (track.codec == codec_id::opus && track.clock_rate == 48'000 &&
                                                 (track.channel_count == 1 || track.channel_count == 2) && track.codec_config.empty()) ||
                                                ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) && track.clock_rate == 8'000 &&
                                                 track.channel_count == 1 && track.codec_config.empty())));
}

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
