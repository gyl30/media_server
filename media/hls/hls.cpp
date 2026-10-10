#include <map>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>

#include <boost/asio/steady_timer.hpp>

#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/core/media_stream.h"
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

constexpr auto ended_retention = std::chrono::duration<double>(target_duration_seconds * static_cast<double>(segment_window_size));

void remove_expired_segmenters(std::chrono::steady_clock::time_point now)
{
    std::erase_if(runtime().segmenters,
                  [now](const auto& item)
                  {
                      const auto ended_at = item.second.segmenter->ended_at();
                      return ended_at && now - *ended_at >= ended_retention;
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

std::shared_ptr<hls_segmenter> get_or_create(const std::shared_ptr<media_stream>& stream)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    const auto now = std::chrono::steady_clock::now();
    remove_expired_segmenters(now);

    const auto& stream_id = stream->stream_id();
    auto existing = current.segmenters.find(stream_id);

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
        segmenter->shutdown();
        return {};
    }
    current.segmenters.emplace(std::string(stream_id), entry{.stream = stream, .segmenter = segmenter});
    return segmenter;
}

void shutdown()
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    for (auto& [stream_id, value] : current.segmenters)
    {
        value.segmenter->shutdown();
    }
    current.segmenters.clear();
}

}    // namespace media_server::hls
