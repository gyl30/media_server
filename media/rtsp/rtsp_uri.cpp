#include <utility>

#include <ada.h>

#include "media/rtsp/rtsp_uri.h"

namespace media_server
{

std::string rtsp_path_from_uri(std::string_view uri)
{
    const auto parsed = ada::parse<ada::url_aggregator>(uri);
    const auto authority = uri.find("://");
    const auto path_start = authority == std::string_view::npos ? std::string_view::npos : uri.find('/', authority + 3);
    if (!parsed || parsed->get_protocol() != "rtsp:" || parsed->has_search() || parsed->has_hash() ||
        path_start == std::string_view::npos || parsed->get_pathname() != uri.substr(path_start))
    {
        return {};
    }

    auto path = parsed->get_pathname();
    if (path.starts_with('/'))
    {
        path.remove_prefix(1);
    }
    return std::string(path);
}

std::optional<rtsp_target> parse_rtsp_target(std::string_view uri)
{
    auto stream_id = rtsp_path_from_uri(uri);
    if (stream_id.empty())
    {
        return std::nullopt;
    }

    return rtsp_target{.stream_id = std::move(stream_id)};
}

}    // namespace media_server
