#ifndef MEDIA_CORE_MEDIA_SINK_H
#define MEDIA_CORE_MEDIA_SINK_H

#include "media/core/media_types.h"

namespace media_server
{

class worker_context;

class media_sink
{
   public:
    virtual ~media_sink() = default;

    [[nodiscard]] virtual worker_context& worker() noexcept = 0;
    virtual void on_frame(const media_frame& frame) = 0;
    virtual void on_end() = 0;
};

}    // namespace media_server

#endif
