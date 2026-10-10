#include "http_target.h"

#include <algorithm>

namespace media_server
{

std::optional<ada::url_aggregator> parse_http_target(std::string_view target)
{
    if (!target.starts_with('/') || target.starts_with("//") || target.find('#') != std::string_view::npos ||
        std::any_of(target.begin(), target.end(), [](unsigned char character) { return character <= 0x20 || character == 0x7f; }))
    {
        return std::nullopt;
    }
    static const auto base = *ada::parse<ada::url_aggregator>("http://localhost");
    auto parsed = ada::parse<ada::url_aggregator>(target, &base);
    if (!parsed || parsed->get_pathname() != target.substr(0, target.find('?')))
    {
        return std::nullopt;
    }
    return std::move(*parsed);
}

}    // namespace media_server
