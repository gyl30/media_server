#include <charconv>

#include <boost/url/parse.hpp>

#include "media/rtsp/rtsp_uri.h"

namespace media_server
{

std::string rtsp_stream_name_from_uri(std::string_view uri)
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
        if (value.starts_with("trackID="))
        {
            break;
        }
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(value);
    }
    return result;
}

std::optional<track_id> rtsp_track_id_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return std::nullopt;
    }

    for (const auto segment : parsed->segments())
    {
        const std::string_view value(segment.data(), segment.size());
        constexpr std::string_view prefix = "trackID=";
        if (!value.starts_with(prefix))
        {
            continue;
        }

        const auto text = value.substr(prefix.size());
        track_id id = 0;
        const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error != std::errc{} || pointer != text.data() + text.size())
        {
            return std::nullopt;
        }
        return id;
    }
    return std::nullopt;
}

}    // namespace media_server
