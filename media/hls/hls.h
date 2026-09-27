#ifndef MEDIA_HLS_H
#define MEDIA_HLS_H

#include <memory>
#include <string_view>

#include "config.h"

namespace media_server
{
class hls_segmenter;
class worker_context;
}

namespace media_server::hls
{

void startup(worker_context& worker);

[[nodiscard]] std::shared_ptr<hls_segmenter> get_or_create(std::string_view stream_name, const config& config);

void shutdown();

}    // namespace media_server::hls

#endif
