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

}    // namespace media_server
