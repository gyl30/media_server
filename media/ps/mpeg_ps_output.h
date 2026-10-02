#ifndef MEDIA_PS_MPEG_PS_OUTPUT_H
#define MEDIA_PS_MPEG_PS_OUTPUT_H

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "media/core/media_sink.h"
#include "media/core/worker_sink_dispatcher.h"

struct ps_muxer_t;

namespace media_server
{

class media_stream;
class worker_context;

struct mpeg_ps_frame
{
    track_id track{};
    std::int64_t dts_ns{};
    std::int64_t pts_ns{};
    bool key_frame{};
    byte_buffer payload;
    std::uint32_t media_timestamp{};
};

class mpeg_ps_sink
{
   public:
    virtual ~mpeg_ps_sink() = default;
    [[nodiscard]] virtual worker_context& worker() noexcept = 0;
    virtual void on_ps_frame(const mpeg_ps_frame& frame) = 0;
    virtual void on_end() = 0;
};

class mpeg_ps_output final : public media_sink, public std::enable_shared_from_this<mpeg_ps_output>
{
   public:
    explicit mpeg_ps_output(worker_context& worker);

    [[nodiscard]] static bool supported_tracks(const std::vector<media_track>& tracks);
    [[nodiscard]] bool startup(const std::shared_ptr<media_stream>& source);
    [[nodiscard]] worker_context& worker() noexcept override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    void add_sink(std::shared_ptr<mpeg_ps_sink> sink);
    void remove_sink(mpeg_ps_sink* sink);

   private:
    static void* allocate_packet(void* param, std::size_t bytes);
    static void free_packet(void* param, void* packet);
    static int write_packet(void* param, int stream, void* packet, std::size_t bytes);
    void add_sink_owner(std::shared_ptr<mpeg_ps_sink> sink, worker_context& worker);
    void finish();

    worker_context& worker_;
    std::shared_ptr<media_stream> source_;
    std::unique_ptr<ps_muxer_t, int (*)(ps_muxer_t*)> muxer_;
    std::map<track_id, std::pair<media_kind, int>> mux_tracks_;
    std::shared_ptr<std::vector<std::uint8_t>> packet_;
    bool waiting_for_key_frame_{true};
    worker_sink_dispatcher<mpeg_ps_frame, mpeg_ps_sink, &mpeg_ps_sink::on_ps_frame> dispatcher_;
};

}    // namespace media_server

#endif
