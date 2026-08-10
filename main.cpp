#include "media/core/log.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/http/http_server.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_server.h"
#include "media/webrtc/whep_service.h"

#include <boost/asio.hpp>

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct options
{
    std::uint16_t rtmp_port{1935};
    std::uint16_t rtsp_port{8554};
    std::uint16_t http_port{8080};
    std::string webrtc_address{"127.0.0.1"};
    std::vector<std::pair<std::string, std::string>> rtsp_pulls;
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
              << "  --rtsp-pull <stream_name=rtsp_url>\n";
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

    boost::asio::io_context io;
    media_server::stream_registry registry;
    media_server::hls_service hls(registry);
    media_server::whep_service whep(io, registry, webrtc_address);
    if (!whep.ready())
    {
        spdlog::error("dtls certificate create failed");
        return 2;
    }
    media_server::rtmp_server rtmp(io, registry, parsed->rtmp_port);
    media_server::rtsp_server rtsp(io, registry, parsed->rtsp_port);
    media_server::http_server http(io, registry, hls, whep, parsed->http_port);

    if (const auto error = rtmp.start())
    {
        spdlog::error("rtmp listen failed port {} error {}", parsed->rtmp_port, error.message());
        return 2;
    }
    if (const auto error = rtsp.start())
    {
        spdlog::error("rtsp listen failed port {} error {}", parsed->rtsp_port, error.message());
        return 2;
    }
    if (const auto error = http.start())
    {
        spdlog::error("http listen failed port {} error {}", parsed->http_port, error.message());
        return 2;
    }

    std::vector<std::weak_ptr<media_server::rtsp_input_session>> pulls;
    for (const auto& [name, url] : parsed->rtsp_pulls)
    {
        auto pull = std::make_shared<media_server::rtsp_input_session>(io, registry, name, url);
        if (!pull->start())
        {
            spdlog::error("rtsp pull start failed stream {}", name);
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

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
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
            http.close();
            whep.close();
            rtsp.close();
            rtmp.close();
        });

    io.run();
    return 0;
}
