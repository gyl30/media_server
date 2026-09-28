#include <map>
#include <mutex>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/codec/av1_video_egress.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace
{
struct egress_key
{
    const media_stream* source{};
    std::optional<av1_encoding_parameters> parameters;

    auto operator<=>(const egress_key&) const = default;
};

std::mutex egress_mutex;
std::map<egress_key, std::weak_ptr<av1_video_egress>> egresses;
}    // namespace

av1_video_egress::av1_video_egress(
    std::shared_ptr<media_stream> source, worker_context& worker, std::optional<av1_encoding_parameters> parameters)
    : worker_(worker), source_(std::move(source)), output_(std::make_shared<media_stream>(source_->name(), worker)),
      parameters_(parameters)
{
}

std::shared_ptr<media_stream> av1_video_egress::stream() const noexcept { return output_; }

bool av1_video_egress::startup_transcoder(const media_track& track)
{
    auto transcoder = std::make_unique<video_transcoder>();
    if (!transcoder->startup(video_transcoder_config{
            .input_codec = track.codec,
            .output_codec = codec_id::av1,
            .input_codec_config = track.codec_config,
            .av1 = parameters_,
        }))
    {
        return false;
    }
    transcoder_ = std::move(transcoder);
    waiting_for_source_key_frame_ = true;
    return true;
}

bool av1_video_egress::startup(const std::vector<media_track>& tracks)
{
    std::vector<media_track> output_tracks;
    output_tracks.reserve(tracks.size());
    for (const auto& track : tracks)
    {
        source_tracks_.emplace(track.id, track);
        auto output_track = track;
        if (track.kind == media_kind::video)
        {
            if (video_track_id_ != 0 || (track.codec != codec_id::h264 && track.codec != codec_id::h265) || !startup_transcoder(track))
            {
                return false;
            }
            video_track_id_ = track.id;
            output_track.codec = codec_id::av1;
            output_track.clock_rate = 90'000;
            output_track.codec_config.clear();
        }
        output_tracks.push_back(std::move(output_track));
    }
    if (video_track_id_ == 0 || !output_->set_tracks(std::move(output_tracks)))
    {
        return false;
    }
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->finish(); });
    if (!shutdown_subscription_)
    {
        return false;
    }
    source_->add_reader(self, worker_);
    return true;
}

void av1_video_egress::finish()
{
    if (ended_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    remove_reader();
    output_->end();
    transcoder_.reset();
    source_.reset();
    shutdown_subscription_.reset();
}

void av1_video_egress::on_tracks(media_tracks_ptr tracks)
{
    if (ended_.load(std::memory_order_acquire) || !tracks)
    {
        return;
    }

    if (tracks->size() != source_tracks_.size())
    {
        finish();
        return;
    }
    for (const auto& track : *tracks)
    {
        auto previous = source_tracks_.find(track.id);
        if (previous == source_tracks_.end() || previous->second.kind != track.kind || previous->second.codec != track.codec ||
            previous->second.clock_rate != track.clock_rate || previous->second.channel_count != track.channel_count ||
            previous->second.codec_config != track.codec_config)
        {
            finish();
            return;
        }
    }
    if (!reading_)
    {
        reading_ = true;
        on_media_available();
    }
}

void av1_video_egress::on_media_available()
{
    while (!ended_.load(std::memory_order_acquire))
    {
        auto entry = read();
        if (!entry)
        {
            return;
        }
        const auto track = source_tracks_.find(entry->frame.track);
        if (track == source_tracks_.end())
        {
            continue;
        }
        if (entry->frame.track != video_track_id_)
        {
            output_->publish(entry->frame);
            continue;
        }
        if (waiting_for_source_key_frame_)
        {
            if (!entry->frame.key_frame)
            {
                continue;
            }
            waiting_for_source_key_frame_ = false;
        }
        std::vector<media_frame> encoded;
        if (!transcoder_->transcode(entry->frame, encoded))
        {
            spdlog::error("shared av1 transcode failed track {}", entry->frame.track);
            finish();
            return;
        }
        for (auto& frame : encoded)
        {
            output_->publish(std::move(frame));
        }
    }
}

void av1_video_egress::on_end() { finish(); }

std::shared_ptr<av1_video_egress> acquire_av1_video_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, std::optional<av1_encoding_parameters> parameters)
{
    if (!source)
    {
        return {};
    }
    const egress_key key{source.get(), parameters};
    std::scoped_lock lock(egress_mutex);
    std::erase_if(egresses, [](const auto& entry) { return entry.second.expired(); });
    if (const auto it = egresses.find(key); it != egresses.end())
    {
        if (auto existing = it->second.lock(); existing && existing->viewers_ != 0 && !existing->ended_.load(std::memory_order_acquire))
        {
            ++existing->viewers_;
            return existing;
        }
    }

    auto created = std::shared_ptr<av1_video_egress>(new av1_video_egress(source, worker, parameters));
    if (!created->startup(source->tracks()))
    {
        created->finish();
        return {};
    }
    created->viewers_ = 1;
    egresses[key] = created;
    return created;
}

void release_av1_video_egress(std::shared_ptr<av1_video_egress>& egress)
{
    auto released = std::exchange(egress, {});
    if (!released)
    {
        return;
    }
    {
        std::scoped_lock lock(egress_mutex);
        if (--released->viewers_ != 0)
        {
            return;
        }
    }
    if (released->ended_.load(std::memory_order_acquire))
    {
        return;
    }
    released->remove_reader();
    boost::asio::post(released->worker_.io(),
                      [weak = std::weak_ptr<av1_video_egress>(released)]()
                      {
                          if (const auto self = weak.lock())
                          {
                              self->finish();
                          }
                      });
}

}    // namespace media_server
