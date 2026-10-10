#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include "media/ps/mpeg_ps_output.h"
#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/codec/codec_utils.h"

extern "C"
{
#include "mpeg-ps.h"
}

namespace media_server
{
mpeg_ps_output::mpeg_ps_output(worker_context& worker) : worker_(worker), muxer_(nullptr, &ps_muxer_destroy), dispatcher_(worker)
{
}

worker_context& mpeg_ps_output::worker() noexcept { return worker_; }

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

bool mpeg_ps_output::startup(const std::shared_ptr<media_stream>& source)
{
    if (!source || !supported_tracks(source->tracks()))
    {
        return false;
    }
    source_ = source;
    const ps_muxer_func_t callbacks{allocate_packet, free_packet, write_packet};
    muxer_.reset(ps_muxer_create(&callbacks, this));
    if (!muxer_)
    {
        spdlog::error("mpeg ps muxer create failed stream {}", source->stream_id());
        return false;
    }
    for (const auto& track : source->tracks())
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
            spdlog::error("mpeg ps muxer add track failed stream {} track {}", source->stream_id(), track.id);
            return false;
        }
        mux_tracks_.emplace(track.id, std::pair{track.kind, id});
    }
    source->add_sink(shared_from_this());
    return true;
}

void mpeg_ps_output::on_frame(const media_frame& frame)
{
    if (!muxer_ || !frame.payload)
    {
        return;
    }
    const auto iterator = mux_tracks_.find(frame.track);
    if (iterator == mux_tracks_.end())
    {
        return;
    }
    const auto& [kind, id] = iterator->second;
    if (waiting_for_key_frame_ && (kind != media_kind::video || !frame.key_frame))
    {
        return;
    }
    const auto pts = ns_to_90khz(frame.pts_ns);
    if (ps_muxer_input(muxer_.get(), id, frame.key_frame ? MPEG_FLAG_IDR_FRAME : 0, pts, ns_to_90khz(frame.dts_ns),
                       frame.payload->data(), frame.payload->size()) < 0)
    {
        spdlog::error("mpeg ps muxer input failed stream {} track {}", source_->stream_id(), frame.track);
        finish();
        return;
    }
    waiting_for_key_frame_ = false;
    dispatcher_.publish({.track = frame.track,
                         .dts_ns = frame.dts_ns,
                         .pts_ns = frame.pts_ns,
                         .key_frame = frame.key_frame,
                         .payload = std::move(packet_),
                         .media_timestamp = static_cast<std::uint32_t>(pts)});
}

void mpeg_ps_output::on_end() { finish(); }

void mpeg_ps_output::add_sink(std::shared_ptr<mpeg_ps_sink> sink)
{
    if (!sink)
    {
        return;
    }
    auto* target = &sink->worker();
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink = std::move(sink), target]() mutable
                          { self->add_sink_owner(std::move(sink), *target); });
}

void mpeg_ps_output::add_sink_owner(std::shared_ptr<mpeg_ps_sink> sink, worker_context& worker)
{
    if (!source_ || !muxer_)
    {
        boost::asio::post(worker.io(), [sink = std::move(sink)]() { sink->on_end(); });
        return;
    }
    dispatcher_.add(std::move(sink), worker);
}

void mpeg_ps_output::remove_sink(mpeg_ps_sink* sink)
{
    if (!sink)
    {
        return;
    }
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink]() { self->dispatcher_.remove(sink); });
}

void mpeg_ps_output::finish()
{
    if (source_)
    {
        source_->remove_sink(this);
        source_.reset();
    }
    dispatcher_.end();
    packet_.reset();
    muxer_.reset();
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
