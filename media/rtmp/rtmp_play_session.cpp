#include <utility>

#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_play_session.h"

extern "C"
{
#include "flv-proto.h"
}

namespace media_server
{

rtmp_play_session::rtmp_play_session(worker_context& worker,
                                     std::shared_ptr<media_stream> stream,
                                     packet_handler on_packet,
                                     end_handler handle_source_end)
    : worker_(worker), stream_(std::move(stream)), packet_handler_(std::move(on_packet)), end_handler_(std::move(handle_source_end))
{
}

bool rtmp_play_session::prepare()
{
    output_ = stream_->flv_output();
    return output_ != nullptr;
}

void rtmp_play_session::startup() { output_->add_sink(shared_from_this()); }

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
    stream_.reset();
    output_.reset();
    packet_handler_ = {};
    end_handler_ = {};
}

void rtmp_play_session::on_flv_tag(const flv_tag& tag)
{
    if (!stream_)
    {
        return;
    }

    if (!started_)
    {
        if (!tag.key_frame)
        {
            return;
        }
        started_ = true;
        const auto& headers = *tag.headers;
        if ((headers.video && packet_handler_(FLV_TYPE_VIDEO, *headers.video, tag.timestamp) != 0) ||
            (headers.audio && packet_handler_(FLV_TYPE_AUDIO, *headers.audio, tag.timestamp) != 0))
        {
            shutdown();
            end_handler_();
            return;
        }
    }

    if (packet_handler_(tag.type, *tag.data, tag.timestamp) != 0)
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
