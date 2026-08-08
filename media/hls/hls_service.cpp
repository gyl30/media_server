#include "media/hls/hls_service.h"

namespace media_server
{

hls_service::hls_service(stream_registry& registry)
    : registry_(registry)
{
}

std::optional<std::string> hls_service::playlist(std::string_view stream_name)
{
    auto output = get_or_create(stream_name);
    if (!output)
    {
        return std::nullopt;
    }
    return output->playlist(".");
}

std::optional<std::vector<std::uint8_t>> hls_service::segment(
    std::string_view stream_name,
    std::uint64_t sequence)
{
    auto output = get_or_create(stream_name);
    if (!output)
    {
        return std::nullopt;
    }
    return output->segment(sequence);
}

std::optional<std::size_t> hls_service::segment_count(std::string_view stream_name)
{
    auto output = get_or_create(stream_name);
    if (!output)
    {
        return std::nullopt;
    }
    return output->segment_count();
}

std::shared_ptr<hls_output> hls_service::get_or_create(std::string_view stream_name)
{
    auto stream = registry_.find(stream_name);
    if (!stream)
    {
        return {};
    }

    const auto existing = outputs_.find(stream_name);
    if (existing != outputs_.end())
    {
        if (const auto current = existing->second.stream.lock(); current && current.get() == stream.get())
        {
            return existing->second.output;
        }
        outputs_.erase(existing);
    }

    auto output = std::make_shared<hls_output>();
    if (!stream->add_sink(output))
    {
        return {};
    }
    outputs_.emplace(
        std::string(stream_name),
        entry{.stream = stream, .output = output});
    return output;
}

}    // namespace media_server
