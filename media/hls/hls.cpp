#include <map>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>
#include <algorithm>

#include <boost/asio/steady_timer.hpp>

#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/hls/hls_play_session.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"

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

}    // namespace

void startup(worker_context& worker)
{
    worker.spawn([](boost::asio::yield_context yield)
                 {
                     boost::asio::steady_timer timer(yield.get_executor());
                     for (;;)
                     {
                         timer.expires_after(std::chrono::seconds(1));
                         boost::system::error_code error;
                         timer.async_wait(yield[error]);
                         if (error || yield.cancelled() != boost::asio::cancellation_type::none)
                         {
                             return;
                         }
                         auto& current = runtime();
                         std::scoped_lock lock(current.mutex);
                         remove_expired_segmenters(std::chrono::steady_clock::now());
                     }
                 });
}

std::shared_ptr<hls_segmenter> get_or_create(std::string_view stream_name)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    const auto now = std::chrono::steady_clock::now();
    remove_expired_segmenters(now);

    auto existing = current.segmenters.find(stream_name);
    auto stream = stream_registry::instance().find(stream_name);
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
        existing->second.segmenter->shutdown();
        current.segmenters.erase(existing);
    }

    auto segmenter = std::make_shared<hls_segmenter>();
    if (!segmenter->startup(stream))
    {
        return {};
    }
    current.segmenters.emplace(std::string(stream_name), entry{.stream = stream, .segmenter = segmenter});
    return segmenter;
}

void shutdown()
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    for (auto& [stream_name, value] : current.segmenters)
    {
        value.segmenter->shutdown();
    }
    current.segmenters.clear();
}

}    // namespace media_server::hls
