#ifndef MEDIA_RTMP_RTMP_PLAY_SESSION_H
#define MEDIA_RTMP_RTMP_PLAY_SESSION_H

#include <span>
#include <memory>
#include <cstdint>
#include <functional>

#include "media/core/media_stream.h"
#include "media/flv/flv_tag_output.h"

namespace media_server
{

class worker_context;
class rtmp_play_session final : public flv_sink, public std::enable_shared_from_this<rtmp_play_session>
{
   public:
    using packet_handler = std::function<int(int, std::span<const std::uint8_t>, std::uint32_t)>;
    using end_handler = std::function<void()>;

    rtmp_play_session(worker_context& worker, std::shared_ptr<media_stream> stream, packet_handler on_packet, end_handler handle_source_end);

   public:
    [[nodiscard]] bool prepare();
    void startup();
    void shutdown();

    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_flv_tag(const flv_tag& tag) override;
    void on_end() override;

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    packet_handler packet_handler_;
    end_handler end_handler_;
    std::shared_ptr<flv_tag_output> output_;
    bool started_{};
};

}    // namespace media_server

#endif
