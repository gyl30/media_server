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

whep_audio_egress::whep_audio_egress(std::shared_ptr<media_stream> source, worker_context& worker, whep_audio_settings settings)
    : worker_(worker), source_(std::move(source)), output_(std::make_shared<media_stream>(source_->name(), worker)), settings_(settings)
{
}

whep_audio_egress::~whep_audio_egress()
{
    reader_handle().remove();
    if (reason_.load(std::memory_order_acquire) == end_reason::none)
    {
        auto output = std::move(output_);
        boost::asio::post(worker_.io(), [output = std::move(output)]() { output->end(); });
    }
}

std::shared_ptr<media_stream> whep_audio_egress::stream() const noexcept { return output_; }

whep_audio_egress::end_reason whep_audio_egress::reason() const noexcept { return reason_.load(std::memory_order_acquire); }

bool whep_audio_egress::startup(const std::vector<media_track>& tracks)
{
    std::vector<media_track> output_tracks;
    output_tracks.reserve(tracks.size());
    for (const auto& track : tracks)
    {
        initial_versions_.emplace(track.id, track.config_version);
        source_tracks_.emplace(track.id, track);
        auto output_track = track;
        if (track.codec == codec_id::aac)
        {
            auto transcoder = std::make_unique<audio_transcoder>();
            int cutoff = 20'000;
            if (settings_.max_playback_rate <= 8'000)
            {
                cutoff = 4'000;
            }
            else if (settings_.max_playback_rate <= 12'000)
            {
                cutoff = 6'000;
            }
            else if (settings_.max_playback_rate <= 16'000)
            {
                cutoff = 8'000;
            }
            else if (settings_.max_playback_rate <= 24'000)
            {
                cutoff = 12'000;
            }
            if (!transcoder->startup(audio_transcoder_config{
                    .input = {.codec = codec_id::aac, .sample_rate = track.clock_rate, .channel_count = track.channel_count},
                    .output = {.codec = codec_id::opus, .sample_rate = 48'000, .channel_count = static_cast<std::uint16_t>(settings_.channels)},
                    .input_codec_config = track.codec_config,
                    .output_bit_rate = settings_.bitrate,
                    .output_cutoff = cutoff,
                }))
            {
                return false;
            }
            transcoders_.emplace(track.id, std::move(transcoder));
            output_track.codec = codec_id::opus;
            output_track.clock_rate = 48'000;
            output_track.channel_count = static_cast<std::uint16_t>(settings_.channels);
            output_track.codec_config.clear();
        }
        output_tracks.push_back(std::move(output_track));
    }
    if (!output_->set_tracks(std::move(output_tracks)))
    {
        return false;
    }
    source_->add_reader(shared_from_this(), worker_);
    return true;
}

bool whep_audio_egress::matches(const std::vector<media_track>& tracks) const
{
    if (reason() != end_reason::none || tracks.size() != initial_versions_.size())
    {
        return false;
    }
    return std::ranges::all_of(tracks,
                               [this](const media_track& track)
                               {
                                   const auto it = initial_versions_.find(track.id);
                                   return it != initial_versions_.end() && it->second == track.config_version;
                               });
}

void whep_audio_egress::finish(end_reason reason)
{
    auto expected = end_reason::none;
    if (!reason_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
    {
        return;
    }
    reader_handle().remove();
    output_->end();
}

void whep_audio_egress::on_tracks(media_track_snapshot_ptr tracks)
{
    if (reason() != end_reason::none || !tracks || tracks->revision <= source_revision_)
    {
        return;
    }
    source_revision_ = tracks->revision;
    for (const auto& track : tracks->tracks)
    {
        auto it = source_tracks_.find(track.id);
        if (it == source_tracks_.end())
        {
            finish(end_reason::source_changed);
            return;
        }
        if (it->second.config_version == track.config_version)
        {
            continue;
        }
        if (track.codec == codec_id::aac)
        {
            finish(end_reason::source_changed);
            return;
        }
        if (!output_->update_track(track))
        {
            finish(end_reason::source_changed);
            return;
        }
        it->second = track;
    }
    if (!reading_)
    {
        reading_ = true;
        reader_handle().async_read(cursor_);
    }
}

void whep_audio_egress::on_read(media_read_batch batch)
{
    on_tracks(batch.tracks);
    if (reason() != end_reason::none)
    {
        return;
    }
    cursor_ = batch.next_cursor;
    for (const auto& entry : batch.entries)
    {
        const auto track = source_tracks_.find(entry.frame.track);
        if (track == source_tracks_.end() || track->second.config_version != entry.config_version)
        {
            continue;
        }
        const auto transcoder = transcoders_.find(entry.frame.track);
        if (transcoder == transcoders_.end())
        {
            output_->publish(entry.frame);
            continue;
        }

        std::vector<media_frame> encoded;
        if (!transcoder->second->transcode(entry.frame, encoded))
        {
            spdlog::error("whep shared audio transcode failed track {}", entry.frame.track);
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
    reader_handle().async_read(cursor_);
}

void whep_audio_egress::on_end() { finish(end_reason::source_ended); }

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
        if (auto existing = it->second.lock(); existing && existing->matches(tracks))
        {
            return existing;
        }
    }

    auto created = std::shared_ptr<whep_audio_egress>(new whep_audio_egress(source, worker, settings));
    if (!created->startup(tracks))
    {
        return {};
    }
    egresses[key] = created;
    return created;
}

}    // namespace media_server
