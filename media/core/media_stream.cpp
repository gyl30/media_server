#include <map>
#include <utility>
#include <algorithm>

#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>

#include "media/core/media_stream.h"
#include "media/ps/mpeg_ps_output.h"
#include "media/flv/flv_tag_output.h"
#include "media/net/worker_context.h"
#include "media/codec/codec_utils.h"

namespace media_server
{
media_stream::media_stream(std::string stream_id, worker_context& worker) : stream_id_(std::move(stream_id)), worker_(worker), dispatcher_(worker) {}
media_stream::~media_stream() = default;

const std::string& media_stream::stream_id() const noexcept { return stream_id_; }
worker_context& media_stream::worker() const noexcept { return worker_; }
const std::vector<media_track>& media_stream::tracks() const noexcept { return tracks_; }

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    if (ended_ || !tracks_.empty() || tracks.empty())
    {
        return false;
    }
    std::map<track_id, bool> ids;
    for (const auto& track : tracks)
    {
        if (track.id == 0 || !ids.emplace(track.id, true).second)
        {
            return false;
        }
    }
    tracks_ = std::move(tracks);
    return true;
}

void media_stream::set_opus_stream(std::shared_ptr<media_stream> stream) { opus_stream_ = std::move(stream); }

const std::shared_ptr<media_stream>& media_stream::opus_stream() const noexcept { return opus_stream_; }

void media_stream::add_sink(std::shared_ptr<media_sink> sink)
{
    if (!sink)
    {
        return;
    }
    auto* target = &sink->worker();
    boost::asio::dispatch(worker_.io(),
                          [self = shared_from_this(), sink = std::move(sink), target]() mutable
                          {
                              if (self->ended_)
                              {
                                  boost::asio::post(target->io(), [sink = std::move(sink)]() { sink->on_end(); });
                                  return;
                              }
                              self->dispatcher_.add(std::move(sink), *target);
                          });
}

void media_stream::remove_sink(media_sink* sink)
{
    if (!sink)
    {
        return;
    }
    boost::asio::dispatch(worker_.io(), [self = shared_from_this(), sink]() { self->dispatcher_.remove(sink); });
}

bool media_stream::has_sinks() const { return dispatcher_.has_live_sinks(); }

void media_stream::publish(media_frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty())
    {
        return;
    }
    const auto track = std::find_if(tracks_.begin(), tracks_.end(), [&frame](const auto& value) { return value.id == frame.track; });
    if (track == tracks_.end())
    {
        return;
    }
    if ((track->codec == codec_id::h264 || track->codec == codec_id::h265) && !frame.length_prefixed)
    {
        frame.length_prefixed = std::make_shared<length_prefixed_payload>();
    }
    dispatcher_.publish(frame);
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    ended_ = true;
    dispatcher_.end();
}

std::shared_ptr<mpeg_ps_output> media_stream::ps_output()
{
    std::scoped_lock lock(output_mutex_);
    if (auto output = ps_output_.lock())
    {
        return output;
    }
    auto output = std::make_shared<mpeg_ps_output>(worker_);
    if (!output->startup(shared_from_this()))
    {
        return {};
    }
    ps_output_ = output;
    return output;
}

std::shared_ptr<flv_tag_output> media_stream::flv_output()
{
    std::scoped_lock lock(output_mutex_);
    if (auto output = flv_output_.lock())
    {
        return output;
    }
    auto output = std::make_shared<flv_tag_output>(worker_);
    if (!output->startup(shared_from_this()))
    {
        return {};
    }
    flv_output_ = output;
    return output;
}

}    // namespace media_server
