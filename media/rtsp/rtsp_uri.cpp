#include <utility>

#include <boost/url/parse.hpp>

#include "media/rtsp/rtsp_uri.h"

namespace media_server
{

std::string rtsp_path_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return {};
    }

    std::string result;
    for (const auto segment : parsed->segments())
    {
        const std::string value(segment);
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(value);
    }
    return result;
}

std::optional<rtsp_target> parse_rtsp_target(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed || parsed->has_fragment())
    {
        return std::nullopt;
    }

    std::string stream_name;
    for (const auto segment : parsed->segments())
    {
        if (!stream_name.empty())
        {
            stream_name.push_back('/');
        }
        stream_name.append(segment);
    }
    if (stream_name.empty())
    {
        return std::nullopt;
    }

    return rtsp_target{.stream_name = std::move(stream_name)};
}

}    // namespace media_server
