#ifndef MEDIA_SERVER_CONFIG_H
#define MEDIA_SERVER_CONFIG_H

#include <string>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace media_server
{

struct config
{
    std::uint16_t rtmp_port{1935};
    std::uint16_t rtsp_port{8554};
    std::uint16_t http_port{8080};
    std::string bind_address{"127.0.0.1"};
    std::string webrtc_address{"127.0.0.1"};
    std::size_t threads{std::max(1U, std::thread::hardware_concurrency())};
    bool help{};
};

int parse_config(int argc, char** argv, config* cfg);

}    // namespace media_server

#endif
