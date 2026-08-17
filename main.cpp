#include "media/codec/output_video_config.h"
#include "media/core/log.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/http/http_server.h"
#include "media/net/io_context_pool.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_server.h"
#include "media/webrtc/whep_service.h"

#include <boost/asio.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

struct options
{
    std::uint16_t rtmp_port{1935};
    std::uint16_t rtsp_port{8554};
    std::uint16_t http_port{8080};
    std::string webrtc_address{"127.0.0.1"};
    std::size_t threads{std::max(1U, std::thread::hardware_concurrency())};
    std::vector<std::pair<std::string, std::string>> rtsp_pulls;
    media_server::output_video_config rtmp_video;
    media_server::output_video_config http_video;
    bool help{};
};

bool parse_port(std::string_view text, std::uint16_t& value)
{
    unsigned int parsed{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || pointer != text.data() + text.size() || parsed > 65'535U)
    {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

std::optional<options> parse_options(int argc, char** argv)
{
    options result;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        auto read_value = [&](std::string_view name) -> std::optional<std::string_view>
        {
            if (argument != name || index + 1 >= argc)
            {
                return std::nullopt;
            }
            ++index;
            return std::string_view(argv[index]);
        };

        if (argument == "--help")
        {
            result.help = true;
            continue;
        }
        if (const auto value = read_value("--rtmp-port"))
        {
            if (!parse_port(*value, result.rtmp_port))
            {
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = read_value("--rtsp-port"))
        {
            if (!parse_port(*value, result.rtsp_port))
            {
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = read_value("--http-port"))
        {
            if (!parse_port(*value, result.http_port))
            {
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = read_value("--webrtc-address"))
        {
            result.webrtc_address = *value;
            continue;
        }
        if (const auto value = read_value("--threads"))
        {
            std::size_t threads{};
            const auto [pointer, error] = std::from_chars(value->data(), value->data() + value->size(), threads);
            if (error != std::errc{} || pointer != value->data() + value->size() || threads == 0)
            {
                return std::nullopt;
            }
            result.threads = threads;
            continue;
        }
        if (const auto value = read_value("--rtmp-video-codec"))
        {
            if (*value == "passthrough")
            {
                result.rtmp_video.codec = media_server::output_video_codec::passthrough;
            }
            else if (*value == "av1")
            {
                result.rtmp_video.codec = media_server::output_video_codec::av1;
            }
            else
            {
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = read_value("--http-video-codec"))
        {
            if (*value == "passthrough")
            {
                result.http_video.codec = media_server::output_video_codec::passthrough;
            }
            else if (*value == "av1")
            {
                result.http_video.codec = media_server::output_video_codec::av1;
            }
            else
            {
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = read_value("--rtsp-pull"))
        {
            const auto equal = value->find('=');
            if (equal == std::string_view::npos || equal == 0 || equal + 1 >= value->size())
            {
                return std::nullopt;
            }
            result.rtsp_pulls.emplace_back(std::string(value->substr(0, equal)), std::string(value->substr(equal + 1)));
            continue;
        }
        return std::nullopt;
    }
    return result;
}

void print_usage()
{
    std::cout << "usage: media_server [options]\n"
              << "  --help\n"
              << "  --rtmp-port <port>\n"
              << "  --rtsp-port <port>\n"
              << "  --http-port <port>\n"
              << "  --webrtc-address <ip>\n"
              << "  --threads <count>\n"
              << "  --rtsp-pull <stream_name=rtsp_url>\n"
              << "  --rtmp-video-codec <passthrough|av1>\n"
              << "  --http-video-codec <passthrough|av1>\n";
}

}    // namespace

int main(int argc, char** argv)
{
    media_server::configure_log_level();

    const auto parsed = parse_options(argc, argv);
    if (!parsed)
    {
        print_usage();
        return 1;
    }
    if (parsed->help)
    {
        print_usage();
        return 0;
    }

    boost::system::error_code address_error;
    const auto webrtc_address = boost::asio::ip::make_address(parsed->webrtc_address, address_error);
    if (address_error)
    {
        spdlog::error("invalid webrtc address {}", parsed->webrtc_address);
        return 1;
    }

    media_server::io_context_pool workers(parsed->threads);
    auto& control_io = workers.context(0);
    media_server::stream_registry registry;
    media_server::hls_service hls(registry, media_server::hls_config{.video = parsed->http_video});
    media_server::whep_service whep(registry, webrtc_address);
    if (!whep.ready())
    {
        spdlog::error("dtls certificate create failed");
        return 2;
    }
    auto rtmp = std::make_shared<media_server::rtmp_server>(workers, registry, parsed->rtmp_port, parsed->rtmp_video);
    auto rtsp = std::make_shared<media_server::rtsp_server>(workers, registry, parsed->rtsp_port);
    auto http = std::make_shared<media_server::http_server>(workers, registry, hls, whep, parsed->http_port, parsed->http_video);

    if (const auto error = rtmp->startup())
    {
        spdlog::error("rtmp listen failed port {} error {}", parsed->rtmp_port, error.message());
        return 2;
    }
    if (const auto error = rtsp->startup())
    {
        spdlog::error("rtsp listen failed port {} error {}", parsed->rtsp_port, error.message());
        return 2;
    }
    if (const auto error = http->startup())
    {
        spdlog::error("http listen failed port {} error {}", parsed->http_port, error.message());
        return 2;
    }

    std::vector<std::weak_ptr<media_server::rtsp_input_session>> pulls;
    for (const auto& [name, url] : parsed->rtsp_pulls)
    {
        auto pull = std::make_shared<media_server::rtsp_input_session>(workers.next(), registry, name, url);
        if (!pull->startup())
        {
            spdlog::error("rtsp pull startup failed stream {}", name);
            return 2;
        }
        pulls.push_back(std::move(pull));
    }

    spdlog::info("rtmp listen {}", parsed->rtmp_port);
    spdlog::info("rtsp listen {}", parsed->rtsp_port);
    spdlog::info("http listen {}", parsed->http_port);
    spdlog::info("rtmp publish play path app/stream");
    spdlog::info("rtsp play path app/stream");
    spdlog::info("http flv path app/stream.flv");
    spdlog::info("hls path hls/app/stream/index.m3u8");
    spdlog::info("whep path whep/app/stream");

    boost::asio::signal_set signals(control_io, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code&, int)
        {
            for (const auto& pull : pulls)
            {
                if (const auto session = pull.lock())
                {
                    session->shutdown();
                }
            }
            pulls.clear();
            http->shutdown();
            http.reset();
            whep.shutdown();
            rtsp->shutdown();
            rtsp.reset();
            rtmp->shutdown();
            rtmp.reset();
            workers.release_work();
        });

    spdlog::info("worker threads {}", workers.size());
    workers.run();
    return 0;
}
