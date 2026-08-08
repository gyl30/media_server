#include "media/core/log.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_service.h"
#include "media/http/http_server.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_server.h"

#include <boost/asio.hpp>

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
    std::vector<std::pair<std::string, std::string>> rtsp_pulls;
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
        auto read_value = [&](std::string_view name) -> std::optional<std::string_view> {
            if (argument != name || index + 1 >= argc)
            {
                return std::nullopt;
            }
            ++index;
            return std::string_view(argv[index]);
        };

        if (argument == "--help")

        {
            return std::nullopt;
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
        if (const auto value = read_value("--rtsp-pull"))
        {
            const auto equal = value->find('=');
            if (equal == std::string_view::npos || equal == 0 || equal + 1 >= value->size())
            {
                return std::nullopt;
            }
            result.rtsp_pulls.emplace_back(
                std::string(value->substr(0, equal)),
                std::string(value->substr(equal + 1)));
            continue;
        }
        return std::nullopt;
    }
    return result;
}

void print_usage()
{
    std::cout
        << "usage: media_server [options]\n"
        << "  --rtmp-port <port>\n"
        << "  --rtsp-port <port>\n"
        << "  --http-port <port>\n"
        << "  --rtsp-pull <stream_name=rtsp_url>\n";
}

}    // namespace

int main(int argc, char** argv)
{
    const auto parsed = parse_options(argc, argv);
    if (!parsed)
    {
        print_usage();
        return argc > 1 ? 1 : 0;
    }

    boost::asio::io_context io;
    media_server::stream_registry registry;
    media_server::hls_service hls(registry);
    media_server::rtmp_server rtmp(io, registry, parsed->rtmp_port);
    media_server::rtsp_server rtsp(io, registry, parsed->rtsp_port);
    media_server::http_server http(io, registry, hls, parsed->http_port);

    rtmp.start();
    rtsp.start();
    http.start();

    std::vector<std::shared_ptr<media_server::rtsp_input_session>> pulls;
    for (const auto& [name, url] : parsed->rtsp_pulls)
    {
        auto pull = std::make_shared<media_server::rtsp_input_session>(io, registry, name, url);
        if (!pull->start())
        {
            media_server::log_line("main", "rtsp pull start failed", name, url);
            return 2;
        }
        pulls.push_back(std::move(pull));
    }

    media_server::log_line("main", "rtmp listen", parsed->rtmp_port);
    media_server::log_line("main", "rtsp listen", parsed->rtsp_port);
    media_server::log_line("main", "http listen", parsed->http_port);
    media_server::log_line("main", "rtmp publish/play path app/stream");
    media_server::log_line("main", "rtsp play path app/stream");
    media_server::log_line("main", "http flv path app/stream.flv");
    media_server::log_line("main", "hls path hls/app/stream/index.m3u8");

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        for (const auto& pull : pulls)
        {
            pull->close();
        }
        http.close();
        rtsp.close();
        rtmp.close();
        io.stop();
    });

    io.run();
    return 0;
}
