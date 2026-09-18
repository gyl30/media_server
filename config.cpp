#include <string>
#include <utility>
#include <charconv>
#include <iostream>
#include <string_view>

#include <boost/url/parse.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/program_options.hpp>

#include "config.h"

namespace media_server
{

namespace
{

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

bool parse_video_transcode_codec(std::string_view text, video_transcode_codec& codec)
{
    if (text == "passthrough")
    {
        codec = video_transcode_codec::passthrough;
        return true;
    }
    if (text == "av1")
    {
        codec = video_transcode_codec::av1;
        return true;
    }
    return false;
}

bool valid_http_base_url(std::string_view text)
{
    const auto parsed = boost::urls::parse_uri(text);
    if (!parsed)
    {
        return false;
    }
    const auto url = *parsed;
    return url.scheme() == "http" && !url.host().empty() && !url.has_userinfo() && (url.path().empty() || url.path() == "/") && !url.has_query() &&
           !url.has_fragment();
}

void print_usage(const boost::program_options::options_description& options) { std::cout << "usage: media_server [options]\n" << options << '\n'; }

}    // namespace

int parse_config(int argc, char** argv, config* cfg)
{
    config result;
    std::string rtmp_port{std::to_string(result.rtmp_port)};
    std::string rtsp_port{std::to_string(result.rtsp_port)};
    std::string http_port{std::to_string(result.http_port)};
    std::string threads{std::to_string(result.threads)};
    std::string rtmp_video_codec;
    std::string rtsp_video_codec;
    std::string http_video_codec;
    std::string whep_video_codec;

    boost::program_options::options_description options("options");
    options.add_options()("help", "show help")("rtmp-port", boost::program_options::value<std::string>(&rtmp_port), "rtmp listen port")(
        "rtsp-port", boost::program_options::value<std::string>(&rtsp_port), "rtsp listen port")(
        "http-port", boost::program_options::value<std::string>(&http_port), "http listen port")(
        "bind-address", boost::program_options::value<std::string>(&result.bind_address), "server listen address")(
        "webrtc-address", boost::program_options::value<std::string>(&result.webrtc_address), "webrtc address")(
        "threads", boost::program_options::value<std::string>(&threads), "worker thread count")(
        "rtmp-video-codec", boost::program_options::value<std::string>(&rtmp_video_codec), "passthrough|av1")(
        "rtsp-video-codec", boost::program_options::value<std::string>(&rtsp_video_codec), "passthrough|av1")(
        "http-video-codec", boost::program_options::value<std::string>(&http_video_codec), "passthrough|av1")(
        "whep-video-codec", boost::program_options::value<std::string>(&whep_video_codec), "passthrough|av1");
    options.add_options()("signaling-url", boost::program_options::value<std::string>(&result.signaling_url), "GB28181 signaling base URL")(
        "server-id", boost::program_options::value<std::string>(&result.server_id), "stable media server identity")(
        "control-url", boost::program_options::value<std::string>(&result.control_url), "media server control URL")(
        "media-ip", boost::program_options::value<std::string>(&result.media_ip), "media address advertised to GB28181 devices");

    boost::program_options::variables_map values;
    try
    {
        const auto parsed =
            boost::program_options::command_line_parser(argc, argv)
                .options(options)
                .style(boost::program_options::command_line_style::default_style & ~boost::program_options::command_line_style::allow_guessing)
                .run();
        if (!boost::program_options::collect_unrecognized(parsed.options, boost::program_options::include_positional).empty())
        {
            print_usage(options);
            return 1;
        }
        boost::program_options::store(parsed, values);
        boost::program_options::notify(values);
    }
    catch (const boost::program_options::error&)
    {
        print_usage(options);
        return 1;
    }

    if (!parse_port(rtmp_port, result.rtmp_port) || !parse_port(rtsp_port, result.rtsp_port) || !parse_port(http_port, result.http_port))
    {
        print_usage(options);
        return 1;
    }

    std::size_t thread_count{};
    const auto [thread_pointer, thread_error] = std::from_chars(threads.data(), threads.data() + threads.size(), thread_count);
    if (thread_error != std::errc{} || thread_pointer != threads.data() + threads.size() || thread_count == 0)
    {
        print_usage(options);
        return 1;
    }
    result.threads = thread_count;

    boost::system::error_code bind_address_error;
    const auto bind_address = boost::asio::ip::make_address(result.bind_address, bind_address_error);
    boost::system::error_code webrtc_address_error;
    const auto webrtc_address = boost::asio::ip::make_address(result.webrtc_address, webrtc_address_error);
    if (bind_address_error || bind_address.is_unspecified() || webrtc_address_error || webrtc_address.is_unspecified())
    {
        print_usage(options);
        return 1;
    }

    if ((values.count("rtmp-video-codec") != 0U && !parse_video_transcode_codec(rtmp_video_codec, result.rtmp_video.codec)) ||
        (values.count("rtsp-video-codec") != 0U && !parse_video_transcode_codec(rtsp_video_codec, result.rtsp_video.codec)) ||
        (values.count("http-video-codec") != 0U && !parse_video_transcode_codec(http_video_codec, result.http_video.codec)) ||
        (values.count("whep-video-codec") != 0U && !parse_video_transcode_codec(whep_video_codec, result.whep_video.codec)))
    {
        print_usage(options);
        return 1;
    }

    const bool has_signaling_value =
        !result.signaling_url.empty() || !result.server_id.empty() || !result.control_url.empty() || !result.media_ip.empty();
    if (has_signaling_value && (result.signaling_url.empty() || result.server_id.empty() || result.control_url.empty() || result.media_ip.empty() ||
                                !valid_http_base_url(result.signaling_url) || !valid_http_base_url(result.control_url)))
    {
        print_usage(options);
        return 1;
    }
    if (has_signaling_value)
    {
        boost::system::error_code media_ip_error;
        const auto media_ip = boost::asio::ip::make_address(result.media_ip, media_ip_error);
        if (media_ip_error || media_ip.is_unspecified())
        {
            print_usage(options);
            return 1;
        }
    }

    if (values.count("help") != 0U)
    {
        result.help = true;
        print_usage(options);
    }

    *cfg = std::move(result);
    return 0;
}

}    // namespace media_server
