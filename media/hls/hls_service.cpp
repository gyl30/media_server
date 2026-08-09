#include "media/hls/hls_service.h"

#include <algorithm>

namespace media_server
{

hls_service::hls_service(stream_registry& registry, hls_config config)
    : registry_(registry), config_(config)
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
    const auto now = std::chrono::steady_clock::now();
    remove_expired_outputs(now);

    auto existing = outputs_.find(stream_name);
    auto stream = registry_.find(stream_name);
    if (!stream)
    {
        if (existing == outputs_.end())
        {
            return {};
        }

        const auto ended_at = existing->second.output->ended_at();
        if (!ended_at || now - *ended_at >= ended_retention())
        {
            outputs_.erase(existing);
            return {};
        }
        return existing->second.output;
    }

    if (existing != outputs_.end())
    {
        if (const auto current = existing->second.stream.lock(); current && current.get() == stream.get())
        {
            return existing->second.output;
        }
        outputs_.erase(existing);
    }

    auto output = std::make_shared<hls_output>(config_);
    if (!stream->add_sink(output))
    {
        return {};
    }
    outputs_.emplace(
        std::string(stream_name),
        entry{.stream = stream, .output = output});
    return output;
}

void hls_service::remove_expired_outputs(std::chrono::steady_clock::time_point now)
{
    const auto retention = ended_retention();
    std::erase_if(outputs_, [now, retention](const auto& item) {
        const auto ended_at = item.second.output->ended_at();
        return ended_at && now - *ended_at >= retention;
    });
}

std::chrono::steady_clock::duration hls_service::ended_retention() const
{
    const auto window_size = std::max<std::size_t>(config_.window_size, 1U);
    const auto seconds = std::max(config_.target_duration_seconds, 0.001) * static_cast<double>(window_size);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

}    // namespace media_server
