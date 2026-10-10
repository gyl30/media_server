#include <utility>

#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_play_session.h"

namespace media_server
{

rtmp_play_session::rtmp_play_session(worker_context& worker,
                                     std::shared_ptr<media_stream> stream,
                                     flv_muxer::packet_handler packet_handler,
                                     end_handler handle_source_end)
    : worker_(worker),
      stream_(std::move(stream)),
      packet_handler_(std::move(packet_handler)),
      muxer_([this](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
             {
                 if (initial_packets_)
                 {
                     initial_packets_->push_back(packet{type, {data.begin(), data.end()}, timestamp});
                     return 0;
                 }
                 return packet_handler_(type, data, timestamp);
             }),
      end_handler_(std::move(handle_source_end))
{
}

bool rtmp_play_session::prepare()
{
    for (const auto& track : stream_->tracks())
    {
        if (!muxer_.on_track(track))
        {
            return false;
        }
        if (track.kind == media_kind::video)
        {
            waiting_video_track_ = track.id;
        }
    }
    return true;
}

bool rtmp_play_session::startup()
{
    for (const auto& entry : *initial_packets_)
    {
        if (packet_handler_(entry.type, entry.data, entry.timestamp) != 0)
        {
            return false;
        }
    }
    initial_packets_.reset();
    stream_->add_sink(shared_from_this());
    return true;
}

void rtmp_play_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_play_session::safe_shutdown()
{
    if (!stream_)
    {
        return;
    }
    stream_->remove_sink(this);
    stream_.reset();
    waiting_video_track_.reset();
    initial_packets_.reset();
    muxer_.shutdown();
    packet_handler_ = {};
    end_handler_ = {};
}

void rtmp_play_session::on_frame(const media_frame& frame)
{
    if (!stream_)
    {
        return;
    }

    if (waiting_video_track_)
    {
        if (frame.track != *waiting_video_track_ || !frame.key_frame)
        {
            return;
        }
        waiting_video_track_.reset();
    }

    if (!muxer_.on_frame(frame))
    {
        shutdown();
        end_handler_();
    }
}

void rtmp_play_session::on_end()
{
    if (!stream_)
    {
        return;
    }
    end_handler_();
}

}    // namespace media_server
