#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>

#include "config.h"

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

int parse(std::vector<std::string> arguments, media_server::config* cfg)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    return media_server::parse_config(static_cast<int>(argv.size()), argv.data(), cfg);
}

void test_defaults()
{
    media_server::config cfg;
    require(parse({"media_server"}, &cfg) == 0, "default config parse");
    require(cfg.rtmp_port == 1935, "default rtmp port");
    require(cfg.rtsp_port == 8554, "default rtsp port");
    require(cfg.http_port == 8080, "default http port");
    require(cfg.bind_address == "127.0.0.1", "default bind address");
    require(cfg.webrtc_address == "127.0.0.1", "default webrtc address");
    require(cfg.threads > 0, "default threads");
    require(cfg.rtsp_pulls.empty(), "default rtsp pulls");
    require(cfg.signaling_url.empty(), "default signaling disabled");
    require(cfg.rtmp_video.codec == media_server::output_video_codec::passthrough, "default rtmp video codec");
    require(cfg.rtsp_video.codec == media_server::output_video_codec::passthrough, "default rtsp video codec");
    require(cfg.http_video.codec == media_server::output_video_codec::passthrough, "default http video codec");
    require(cfg.whep_video.codec == media_server::output_video_codec::passthrough, "default whep video codec");
    require(!cfg.help, "default help");
}

void test_values()
{
    media_server::config cfg;
    require(parse({"media_server",
                   "--rtmp-port",
                   "11935",
                   "--rtsp-port=18554",
                   "--http-port",
                   "18080",
                   "--bind-address",
                   "192.0.2.20",
                   "--webrtc-address",
                   "192.0.2.10",
                   "--threads",
                   "3",
                   "--rtsp-pull",
                   "live/one=rtsp://127.0.0.1/one",
                   "--rtsp-pull=live/two=rtsp://127.0.0.1/two",
                   "--rtmp-video-codec",
                   "av1",
                   "--rtsp-video-codec=av1",
                   "--http-video-codec",
                   "av1",
                   "--whep-video-codec=av1"},
                  &cfg) == 0,
            "explicit config parse");
    require(cfg.rtmp_port == 11935, "explicit rtmp port");
    require(cfg.rtsp_port == 18554, "explicit rtsp port");
    require(cfg.http_port == 18080, "explicit http port");
    require(cfg.bind_address == "192.0.2.20", "explicit bind address");
    require(cfg.webrtc_address == "192.0.2.10", "explicit webrtc address");
    require(cfg.threads == 3, "explicit threads");
    require(cfg.rtsp_pulls ==
                std::vector<std::pair<std::string, std::string>>{{"live/one", "rtsp://127.0.0.1/one"}, {"live/two", "rtsp://127.0.0.1/two"}},
            "explicit rtsp pulls");
    require(cfg.rtmp_video.codec == media_server::output_video_codec::av1, "explicit rtmp video codec");
    require(cfg.rtsp_video.codec == media_server::output_video_codec::av1, "explicit rtsp video codec");
    require(cfg.http_video.codec == media_server::output_video_codec::av1, "explicit http video codec");
    require(cfg.whep_video.codec == media_server::output_video_codec::av1, "explicit whep video codec");

    media_server::config signaling_cfg;
    require(parse({"media_server",
                   "--signaling-url",
                   "http://127.0.0.1:19090",
                   "--server-id",
                   "media-1",
                   "--control-url",
                   "http://127.0.0.1:18080",
                   "--media-ip",
                   "192.0.2.10"},
                  &signaling_cfg) == 0,
            "signaling config parse");
    require(signaling_cfg.signaling_url == "http://127.0.0.1:19090", "signaling url");
    require(signaling_cfg.server_id == "media-1", "signaling server id");
    require(signaling_cfg.control_url == "http://127.0.0.1:18080", "signaling control url");
    require(signaling_cfg.media_ip == "192.0.2.10", "signaling media ip");
}

void test_help()
{
    media_server::config cfg;
    require(parse({"media_server", "--help"}, &cfg) == 0, "help parse");
    require(cfg.help, "help flag");
}

void test_invalid()
{
    const std::vector<std::vector<std::string>> cases{
        {"media_server", "--invalid"},
        {"media_server", "positional"},
        {"media_server", "--rtmp-p", "9000"},
        {"media_server", "--help", "--invalid"},
        {"media_server", "--help", "--threads", "0"},
        {"media_server", "--rtmp-port"},
        {"media_server", "--rtmp-port", "65536"},
        {"media_server", "--rtsp-port", "invalid"},
        {"media_server", "--threads", "0"},
        {"media_server", "--threads", "-1"},
        {"media_server", "--rtmp-port", "-1"},
        {"media_server", "--threads", "invalid"},
        {"media_server", "--rtsp-pull", "missing-equals"},
        {"media_server", "--rtsp-pull", "=rtsp://127.0.0.1/live"},
        {"media_server", "--rtsp-pull", "live="},
        {"media_server", "--rtmp-video-codec", "h264"},
        {"media_server", "--rtsp-video-codec", ""},
        {"media_server", "--http-video-codec", "h265"},
        {"media_server", "--whep-video-codec", "vp9"},
        {"media_server", "--bind-address", "0.0.0.0"},
        {"media_server", "--bind-address", "::"},
        {"media_server", "--bind-address", "invalid"},
        {"media_server", "--webrtc-address", "0.0.0.0"},
        {"media_server", "--webrtc-address", "::"},
        {"media_server", "--signaling-url", "http://127.0.0.1:9090"},
        {"media_server", "--signaling-url", "ftp://127.0.0.1:9090", "--server-id", "media-1", "--control-url", "http://127.0.0.1:8080", "--media-ip", "127.0.0.1"},
        {"media_server", "--signaling-url", "http://127.0.0.1:9090/path", "--server-id", "media-1", "--control-url", "http://127.0.0.1:8080", "--media-ip", "127.0.0.1"},
        {"media_server", "--signaling-url", "http://127.0.0.1:9090", "--server-id", "media-1", "--control-url", "invalid", "--media-ip", "127.0.0.1"},
        {"media_server", "--signaling-url", "http://127.0.0.1:9090", "--server-id", "media-1", "--control-url", "http://127.0.0.1:8080", "--media-ip", "invalid"},
    };

    for (const auto& arguments : cases)
    {
        media_server::config cfg;
        require(parse(arguments, &cfg) != 0, "invalid config rejected");
    }
}

}    // namespace

int main()
{
    test_defaults();
    test_values();
    test_help();
    test_invalid();
    std::cout << "[pass] config tests\n";
    return 0;
}
