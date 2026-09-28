#include <map>
#include <mutex>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/webrtc/whep_audio_egress.h"

namespace media_server
{
namespace
{

struct egress_key
{
    const media_stream* source{};
    int channels{};
    int bitrate{};
    int max_playback_rate{};

    auto operator<=>(const egress_key&) const = default;
};

std::mutex egress_mutex;
std::map<egress_key, std::weak_ptr<whep_audio_egress>> egresses;

}    // namespace

whep_audio_egress::whep_audio_egress(std::shared_ptr<media_stream> source, worker_context& worker)
    : worker_(worker), source_(std::move(source)), output_(std::make_shared<media_stream>(source_->name(), worker))
{
}

std::shared_ptr<media_stream> whep_audio_egress::stream() const noexcept { return output_; }

whep_audio_egress::end_reason whep_audio_egress::reason() const noexcept { return reason_.load(std::memory_order_acquire); }

bool whep_audio_egress::startup(const std::vector<media_track>& tracks, whep_audio_settings settings)
{
    std::vector<media_track> output_tracks;
    output_tracks.reserve(tracks.size());
    for (const auto& track : tracks)
    {
        source_tracks_.emplace(track.id, track);
        auto output_track = track;
        if (track.codec == codec_id::aac)
        {
            auto transcoder = std::make_unique<audio_transcoder>();
            int cutoff = 20'000;
            if (settings.max_playback_rate <= 8'000)
            {
                cutoff = 4'000;
            }
            else if (settings.max_playback_rate <= 12'000)
            {
                cutoff = 6'000;
            }
            else if (settings.max_playback_rate <= 16'000)
            {
                cutoff = 8'000;
            }
            else if (settings.max_playback_rate <= 24'000)
            {
                cutoff = 12'000;
            }
            if (!transcoder->startup(audio_transcoder_config{
                    .input = {.codec = codec_id::aac, .sample_rate = track.clock_rate, .channel_count = track.channel_count},
                    .output = {.codec = codec_id::opus, .sample_rate = 48'000, .channel_count = static_cast<std::uint16_t>(settings.channels)},
                    .input_codec_config = track.codec_config,
                    .output_bit_rate = settings.bitrate,
                    .output_cutoff = cutoff,
                }))
            {
                return false;
            }
            transcoders_.emplace(track.id, std::move(transcoder));
            output_track.codec = codec_id::opus;
            output_track.clock_rate = 48'000;
            output_track.channel_count = static_cast<std::uint16_t>(settings.channels);
            output_track.codec_config.clear();
        }
        output_tracks.push_back(std::move(output_track));
    }
    if (!output_->set_tracks(std::move(output_tracks)))
    {
        return false;
    }
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->finish(end_reason::worker_stopped); });
    if (!shutdown_subscription_)
    {
        return false;
    }
    sink_ = std::make_shared<media_sink>();
    source_->add_sink(sink_, worker_);
    process_media_available();
    return true;
}

bool whep_audio_egress::matches(const std::vector<media_track>& tracks) const
{
    const auto prepared_tracks = output_->tracks();
    if (reason() != end_reason::none || tracks.size() != source_tracks_.size() || prepared_tracks.size() != tracks.size())
    {
        return false;
    }
    return std::ranges::all_of(tracks,
                               [this, &prepared_tracks](const media_track& track)
                               {
                                   const auto it = source_tracks_.find(track.id);
                                   if (it == source_tracks_.end())
                                   {
                                       return false;
                                   }
                                   if (track.codec == codec_id::aac)
                                   {
                                       return it->second.kind == track.kind && it->second.codec == track.codec &&
                                              it->second.clock_rate == track.clock_rate && it->second.channel_count == track.channel_count &&
                                              it->second.codec_config == track.codec_config;
                                   }
                                   const auto prepared = std::ranges::find_if(prepared_tracks,
                                                                              [&track](const media_track& value) { return value.id == track.id; });
                                   return prepared != prepared_tracks.end() && prepared->kind == track.kind && prepared->codec == track.codec &&
                                          prepared->clock_rate == track.clock_rate && prepared->channel_count == track.channel_count &&
                                          prepared->codec_config == track.codec_config;
                               });
}

void whep_audio_egress::finish(end_reason reason)
{
    auto expected = end_reason::none;
    if (!reason_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
    {
        return;
    }
    if (sink_)
    {
        sink_->close();
        sink_.reset();
    }
    output_->end();
    transcoders_.clear();
    source_.reset();
    shutdown_subscription_.reset();
}

void whep_audio_egress::process_media_available()
{
    while (reason() == end_reason::none)
    {
        auto entry = sink_->read();
        if (!entry)
        {
            const auto weak = weak_from_this();
            sink_->async_wait([weak](bool ended)
                       {
                           if (const auto self = weak.lock())
                           {
                               if (ended)
                               {
                                   self->handle_source_end();
                               }
                               else
                               {
                                   self->process_media_available();
                               }
                           }
                       });
            return;
        }
        const auto track = source_tracks_.find(entry->track);
        if (track == source_tracks_.end())
        {
            continue;
        }
        const auto transcoder = transcoders_.find(entry->track);
        if (transcoder == transcoders_.end())
        {
            output_->publish(*entry);
            continue;
        }

        std::vector<media_frame> encoded;
        if (!transcoder->second->transcode(*entry, encoded))
        {
            spdlog::error("whep shared audio transcode failed track {}", entry->track);
            finish(end_reason::transcode_failed);
            return;
        }
        for (auto& frame : encoded)
        {
            frame.pts_ns = ns_to_milliseconds(frame.pts_ns) * 1'000'000;
            frame.dts_ns = frame.pts_ns;
            output_->publish(std::move(frame));
        }
    }
}

void whep_audio_egress::handle_source_end() { finish(end_reason::source_ended); }

std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings)
{
    if (!source)
    {
        return {};
    }
    const auto tracks = source->tracks();
    const egress_key key{source.get(), settings.channels, settings.bitrate, settings.max_playback_rate};
    std::scoped_lock lock(egress_mutex);
    std::erase_if(egresses, [](const auto& entry) { return entry.second.expired(); });
    if (const auto it = egresses.find(key); it != egresses.end())
    {
        if (auto existing = it->second.lock(); existing && existing->viewers_ != 0 && existing->matches(tracks))
        {
            ++existing->viewers_;
            return existing;
        }
    }

    auto created = std::shared_ptr<whep_audio_egress>(new whep_audio_egress(source, worker));
    if (!created->startup(tracks, settings))
    {
        created->finish(whep_audio_egress::end_reason::transcode_failed);
        return {};
    }
    created->viewers_ = 1;
    egresses[key] = created;
    return created;
}

void release_whep_audio_egress(std::shared_ptr<whep_audio_egress>& egress)
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
    if (released->reason() != whep_audio_egress::end_reason::none)
    {
        return;
    }
    if (released->sink_)
    {
        released->sink_->close();
    }
    // worker 的 shutdown subscription 持有 processor，排队请求无需延长其终止后的生命。
    boost::asio::post(released->worker_.io(),
                      [weak = std::weak_ptr<whep_audio_egress>(released)]()
                      {
                          if (const auto self = weak.lock())
                          {
                              self->finish(whep_audio_egress::end_reason::unused);
                          }
                      });
}

}    // namespace media_server
