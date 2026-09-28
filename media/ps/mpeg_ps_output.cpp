#include "media/ps/mpeg_ps_output.h"
#include "media/codec/codec_utils.h"
#include "media/core/media_stream.h"

extern "C"
{
#include "mpeg-ps.h"
}

namespace media_server
{

mpeg_ps_output::mpeg_ps_output(std::string name, worker_context& worker)
    : media_history<mpeg_ps_frame>(std::move(name), worker), muxer_(nullptr, &ps_muxer_destroy)
{
}

bool mpeg_ps_output::supported_tracks(const std::vector<media_track>& tracks)
{
    std::size_t video_count = 0;
    std::size_t audio_count = 0;
    for (const auto& track : tracks)
    {
        if (track.kind == media_kind::video)
        {
            ++video_count;
            if (track.codec != codec_id::h264 && track.codec != codec_id::h265)
            {
                return false;
            }
        }
        else
        {
            ++audio_count;
            if (track.codec != codec_id::aac && track.codec != codec_id::g711a && track.codec != codec_id::g711u)
            {
                return false;
            }
        }
    }
    return video_count == 1 && audio_count <= 1;
}

bool mpeg_ps_output::startup(const std::shared_ptr<media_stream>& source, const std::vector<media_track>& tracks)
{
    if (muxer_ || !supported_tracks(tracks))
    {
        return false;
    }
    const ps_muxer_func_t callbacks{allocate_packet, free_packet, write_packet};
    muxer_.reset(ps_muxer_create(&callbacks, this));
    if (!muxer_)
    {
        failed_.store(true, std::memory_order_release);
        finish();
        return false;
    }
    for (const auto& track : tracks)
    {
        int codec = 0;
        switch (track.codec)
        {
            case codec_id::h264:
                codec = PSI_STREAM_H264;
                break;
            case codec_id::h265:
                codec = PSI_STREAM_H265;
                break;
            case codec_id::aac:
                codec = PSI_STREAM_AAC;
                break;
            case codec_id::g711a:
                codec = PSI_STREAM_AUDIO_G711A;
                break;
            case codec_id::g711u:
                codec = PSI_STREAM_AUDIO_G711U;
                break;
            case codec_id::opus:
                break;
        }
        const auto id = ps_muxer_add_stream(muxer_.get(), codec, nullptr, 0);
        if (id < 0)
        {
            failed_.store(true, std::memory_order_release);
            finish();
            return false;
        }
        mux_tracks_.emplace(track.id, std::pair{track.kind, id});
    }
    if (!set_tracks(tracks))
    {
        failed_.store(true, std::memory_order_release);
        finish();
        return false;
    }
    sink_ = std::make_shared<media_sink>();
    source->add_sink(sink_, source->worker());
    process();
    return true;
}

bool mpeg_ps_output::failed() const noexcept { return failed_.load(std::memory_order_acquire); }

void mpeg_ps_output::process_frame(const media_frame& frame)
{
    if (!muxer_)
    {
        return;
    }
    const auto& [kind, id] = mux_tracks_.at(frame.track);
    if (waiting_for_key_frame_ && (kind != media_kind::video || !frame.key_frame))
    {
        return;
    }
    const auto pts = ns_to_milliseconds(frame.pts_ns) * 90;
    if (ps_muxer_input(muxer_.get(),
                       id,
                       frame.key_frame ? MPEG_FLAG_IDR_FRAME : 0,
                       pts,
                       ns_to_milliseconds(frame.dts_ns) * 90,
                       frame.payload->data(),
                       frame.payload->size()) < 0)
    {
        failed_.store(true, std::memory_order_release);
        finish();
        return;
    }
    waiting_for_key_frame_ = false;
    publish({.track = frame.track,
                      .dts_ns = frame.dts_ns,
                      .pts_ns = frame.pts_ns,
                      .key_frame = frame.key_frame,
                      .payload = std::move(packet_),
                      .media_timestamp = static_cast<std::uint32_t>(pts)});
}

void mpeg_ps_output::finish()
{
    if (sink_)
    {
        sink_->close();
        sink_.reset();
    }
    end();
    packet_.reset();
    muxer_.reset();
}

void mpeg_ps_output::process()
{
    if (!sink_)
    {
        return;
    }
    while (const auto entry = sink_->read())
    {
        process_frame(*entry);
    }
    const auto weak = std::weak_ptr<mpeg_ps_output>(std::static_pointer_cast<mpeg_ps_output>(media_history<mpeg_ps_frame>::shared_from_this()));
    sink_->async_wait([weak](bool ended)
                      {
                          if (const auto self = weak.lock())
                          {
                              if (ended)
                              {
                                  self->finish();
                              }
                              else
                              {
                                  self->process();
                              }
                          }
                      });
}

void* mpeg_ps_output::allocate_packet(void* param, std::size_t bytes)
{
    auto& self = *static_cast<mpeg_ps_output*>(param);
    self.packet_ = std::make_shared<std::vector<std::uint8_t>>(bytes);
    return self.packet_->data();
}

void mpeg_ps_output::free_packet(void*, void*) {}

int mpeg_ps_output::write_packet(void* param, int, void*, std::size_t bytes)
{
    static_cast<mpeg_ps_output*>(param)->packet_->resize(bytes);
    return 0;
}

}    // namespace media_server
