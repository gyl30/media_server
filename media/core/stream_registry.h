#ifndef MEDIA_CORE_STREAM_REGISTRY_H
#define MEDIA_CORE_STREAM_REGISTRY_H

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

#include "media/core/media_stream.h"
#include "media/core/singleton.h"

namespace media_server
{

class stream_registry final
{
    friend class singleton<stream_registry>;

   public:
    bool add(const std::shared_ptr<media_stream>& stream);
    void remove(const media_stream& expected);
    [[nodiscard]] std::shared_ptr<media_stream> find(std::string_view name) const;
    void clear();

   private:
    stream_registry() = default;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<media_stream>, std::less<>> streams_;
};

using registry = singleton<stream_registry>;

}    // namespace media_server

#endif
