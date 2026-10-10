#ifndef MEDIA_HLS_H
#define MEDIA_HLS_H

#include <memory>
#include <cstddef>
#include <string_view>

namespace media_server
{
class hls_segmenter;
class media_stream;
class worker_context;
}

namespace media_server::hls
{

inline constexpr double target_duration_seconds = 2.0;
inline constexpr std::size_t segment_window_size = 6;

void startup(worker_context& worker);

[[nodiscard]] std::shared_ptr<hls_segmenter> get_or_create(const std::shared_ptr<media_stream>& stream);

void shutdown();

}    // namespace media_server::hls

#endif
