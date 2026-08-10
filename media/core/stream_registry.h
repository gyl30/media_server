#ifndef MEDIA_CORE_STREAM_REGISTRY_H
#define MEDIA_CORE_STREAM_REGISTRY_H

#include "media/core/media_stream.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace media_server
{

class stream_registry final
{
   public:
    bool add(const std::shared_ptr<media_stream>& stream);
    void remove(const media_stream& expected);
    [[nodiscard]] std::shared_ptr<media_stream> find(std::string_view name) const;

   private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<media_stream>, std::less<>> streams_;
};

}    // namespace media_server

#endif
