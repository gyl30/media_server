#ifndef MEDIA_SERVER_CONFIG_H
#define MEDIA_SERVER_CONFIG_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "media/codec/output_video_config.h"

namespace media_server
{

struct config
{
    std::uint16_t rtmp_port{1935};
    std::uint16_t rtsp_port{8554};
    std::uint16_t http_port{8080};
    std::string webrtc_address{"127.0.0.1"};
    std::size_t threads{std::max(1U, std::thread::hardware_concurrency())};
    std::vector<std::pair<std::string, std::string>> rtsp_pulls;
    std::string signaling_url;
    std::string server_id;
    std::string control_url;
    std::string media_ip;
    output_video_config rtmp_video;
    output_video_config rtsp_video;
    output_video_config http_video;
    output_video_config whep_video;
    bool help{};
};

int parse_config(int argc, char** argv, config* cfg);

}    // namespace media_server

#endif
