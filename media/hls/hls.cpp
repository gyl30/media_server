#include <map>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>
#include <algorithm>

#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/core/stream_registry.h"

namespace media_server::hls
{
namespace
{

struct entry
{
    std::weak_ptr<media_stream> stream;
    std::shared_ptr<hls_segmenter> segmenter;
};

struct state
{
    std::mutex mutex;
    std::map<std::string, entry, std::less<>> segmenters;
};

state& runtime()
{
    static state value;
    return value;
}

hls_config segmenter_config(const config& application_config) { return hls_config{.video = application_config.http_video}; }

std::chrono::steady_clock::duration ended_retention()
{
    const hls_config config;
    const auto window_size = std::max<std::size_t>(config.window_size, 1U);
    const auto seconds = std::max(config.target_duration_seconds, 0.001) * static_cast<double>(window_size);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

void remove_expired_segmenters(std::chrono::steady_clock::time_point now)
{
    const auto retention = ended_retention();
    std::erase_if(runtime().segmenters,
                  [now, retention](const auto& item)
                  {
                      const auto ended_at = item.second.segmenter->ended_at();
                      return ended_at && now - *ended_at >= retention;
                  });
}

std::shared_ptr<hls_segmenter> get_or_create(std::string_view stream_name, const config& application_config)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    const auto now = std::chrono::steady_clock::now();
    remove_expired_segmenters(now);

    auto existing = current.segmenters.find(stream_name);
    auto stream = registry::instance().find(stream_name);
    if (!stream)
    {
        return existing != current.segmenters.end() ? existing->second.segmenter : std::shared_ptr<hls_segmenter>{};
    }

    if (existing != current.segmenters.end())
    {
        if (const auto current_stream = existing->second.stream.lock(); current_stream && current_stream.get() == stream.get())
        {
            return existing->second.segmenter;
        }
        existing->second.segmenter->on_end();
        current.segmenters.erase(existing);
    }

    auto segmenter = std::make_shared<hls_segmenter>(segmenter_config(application_config));
    stream->add_sink(segmenter);
    current.segmenters.emplace(std::string(stream_name), entry{.stream = stream, .segmenter = segmenter});
    return segmenter;
}

}    // namespace

std::optional<std::string> playlist(std::string_view stream_name, const config& application_config)
{
    const auto segmenter = get_or_create(stream_name, application_config);
    return segmenter ? std::optional<std::string>(segmenter->playlist(".")) : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> init_segment(std::string_view stream_name, const config& application_config)
{
    const auto segmenter = get_or_create(stream_name, application_config);
    return segmenter ? segmenter->init_segment() : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> segment(std::string_view stream_name, std::uint64_t sequence, const config& application_config)
{
    const auto segmenter = get_or_create(stream_name, application_config);
    return segmenter ? segmenter->segment(sequence) : std::nullopt;
}

std::optional<std::size_t> segment_count(std::string_view stream_name, const config& application_config)
{
    const auto segmenter = get_or_create(stream_name, application_config);
    return segmenter ? std::optional<std::size_t>(segmenter->segment_count()) : std::nullopt;
}

void shutdown()
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    for (auto& [stream_name, value] : current.segmenters)
    {
        static_cast<void>(stream_name);
        value.segmenter->on_end();
    }
    current.segmenters.clear();
}

}    // namespace media_server::hls
