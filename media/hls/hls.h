#ifndef MEDIA_HLS_H
#define MEDIA_HLS_H

#include <memory>
#include <string_view>

#include "config.h"

namespace media_server
{
class hls_segmenter;
}

namespace media_server::hls
{

[[nodiscard]] std::shared_ptr<hls_segmenter> get_or_create(std::string_view stream_name, const config& config);

void shutdown();

}    // namespace media_server::hls

#endif
