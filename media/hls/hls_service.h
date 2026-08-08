#ifndef MEDIA_HLS_SERVICE_H
#define MEDIA_HLS_SERVICE_H

#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_server
{

class hls_service final
{
   public:
    explicit hls_service(stream_registry& registry);

    [[nodiscard]] std::optional<std::string> playlist(std::string_view stream_name);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> segment(
        std::string_view stream_name,
        std::uint64_t sequence);
    [[nodiscard]] std::optional<std::size_t> segment_count(std::string_view stream_name);

   private:
    struct entry
    {
        std::weak_ptr<media_stream> stream;
        std::shared_ptr<hls_output> output;
    };

    [[nodiscard]] std::shared_ptr<hls_output> get_or_create(std::string_view stream_name);

    stream_registry& registry_;
    std::map<std::string, entry, std::less<>> outputs_;
};

}    // namespace media_server

#endif
