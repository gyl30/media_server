#ifndef MEDIA_FLV_FLV_TAG_OUTPUT_H
#define MEDIA_FLV_FLV_TAG_OUTPUT_H

#include <map>
#include <memory>
#include <cstdint>

#include "media/core/media_sink.h"
#include "media/core/worker_sink_dispatcher.h"

struct flv_muxer_t;

namespace media_server
{

class media_stream;
class worker_context;

struct flv_headers
{
    byte_buffer video;
    byte_buffer audio;
};

struct flv_tag
{
    int type{};
    bool key_frame{};
    byte_buffer data;
    std::uint32_t timestamp{};
    std::shared_ptr<const flv_headers> headers;
};

class flv_sink
{
   public:
    virtual ~flv_sink() = default;
    [[nodiscard]] virtual worker_context& worker() noexcept = 0;
    virtual void on_flv_tag(const flv_tag& tag) = 0;
    virtual void on_end() = 0;
};

class flv_tag_output final : public media_sink, public std::enable_shared_from_this<flv_tag_output>
{
   public:
    explicit flv_tag_output(worker_context& worker);

    [[nodiscard]] bool startup(const std::shared_ptr<media_stream>& source);
    [[nodiscard]] worker_context& worker() noexcept override;
    void on_frame(const media_frame& frame) override;
    void on_end() override;

    void add_sink(std::shared_ptr<flv_sink> sink);

   private:
    static int on_packet(void* param, int type, const void* data, std::size_t bytes, std::uint32_t timestamp);

    [[nodiscard]] bool write_track_config(const media_track& track);
    void finish();

    worker_context& worker_;
    std::shared_ptr<media_stream> source_;
    std::unique_ptr<flv_muxer_t, void (*)(flv_muxer_t*)> muxer_;
    std::map<track_id, codec_id> codecs_;
    std::shared_ptr<const flv_headers> headers_;
    bool has_video_{};
    bool key_frame_{};
    worker_sink_dispatcher<flv_tag, flv_sink, &flv_sink::on_flv_tag> dispatcher_;
};

}    // namespace media_server

#endif
