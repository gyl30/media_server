#ifndef MEDIA_RTMP_RTMP_PLAY_SESSION_H
#define MEDIA_RTMP_RTMP_PLAY_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>

#include "media/flv/flv_muxer.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"

namespace media_server
{

class worker_context;
class rtmp_play_session final : public media_sink, public std::enable_shared_from_this<rtmp_play_session>
{
   public:
    using end_handler = std::function<void()>;

    rtmp_play_session(worker_context& worker,
                      std::shared_ptr<media_stream> stream,
                      flv_muxer::packet_handler packet_handler,
                      end_handler handle_source_end);

   public:
    void startup();
    void shutdown();

    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    flv_muxer muxer_;
    end_handler end_handler_;
    bool waiting_for_key_frame_{true};
    bool closed_{};
    bool source_ended_{};
};

}    // namespace media_server

#endif
