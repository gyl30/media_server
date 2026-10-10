#include <map>
#include <mutex>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/dispatch.hpp>

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
    : worker_(worker), source_(std::move(source)), output_stream_(std::make_shared<media_stream>(source_->stream_id(), worker))
{
}

std::shared_ptr<media_stream> whep_audio_egress::output_stream() const noexcept { return output_stream_; }

bool whep_audio_egress::startup(whep_audio_settings settings)
{
    const auto& tracks = source_->tracks();
    std::vector<media_track> output_tracks;
    output_tracks.reserve(tracks.size());
    for (const auto& track : tracks)
    {
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
    if (!output_stream_->set_tracks(std::move(output_tracks)))
    {
        return false;
    }
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->finish(); });
    if (!shutdown_subscription_)
    {
        return false;
    }
    source_->add_sink(shared_from_this());
    return true;
}

void whep_audio_egress::finish()
{
    if (finished_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    source_->remove_sink(this);
    output_stream_->end();
    transcoders_.clear();
    source_.reset();
    shutdown_subscription_.reset();
}

void whep_audio_egress::on_frame(const media_frame& frame)
{
    if (finished_.load(std::memory_order_acquire))
    {
        return;
    }
    const auto transcoder = transcoders_.find(frame.track);
    if (transcoder == transcoders_.end())
    {
        output_stream_->publish(frame);
        return;
    }

    std::vector<media_frame> encoded;
    if (!transcoder->second->transcode(frame, encoded))
    {
        spdlog::error("whep shared audio transcode failed track {}", frame.track);
        finish();
        return;
    }
    for (auto& encoded_frame : encoded)
    {
        encoded_frame.pts_ns = ns_to_milliseconds(encoded_frame.pts_ns) * 1'000'000;
        encoded_frame.dts_ns = encoded_frame.pts_ns;
        output_stream_->publish(std::move(encoded_frame));
    }
}

void whep_audio_egress::on_end() { finish(); }

std::shared_ptr<whep_audio_egress> acquire_whep_audio_egress(
    const std::shared_ptr<media_stream>& source, worker_context& worker, whep_audio_settings settings)
{
    const egress_key key{source.get(), settings.channels, settings.bitrate, settings.max_playback_rate};
    std::scoped_lock lock(egress_mutex);
    std::erase_if(egresses, [](const auto& entry) { return entry.second.expired(); });
    if (const auto it = egresses.find(key); it != egresses.end())
    {
        if (auto existing = it->second.lock(); existing && existing->viewers_ != 0 && !existing->finished_.load(std::memory_order_acquire))
        {
            ++existing->viewers_;
            return existing;
        }
    }

    auto created = std::shared_ptr<whep_audio_egress>(new whep_audio_egress(source, worker));
    if (!created->startup(settings))
    {
        created->finish();
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
    if (released->finished_.load(std::memory_order_acquire))
    {
        return;
    }
    // worker 的 shutdown subscription 持有 processor，排队请求无需延长其终止后的生命。
    boost::asio::dispatch(released->worker_.io(),
                      [weak = std::weak_ptr<whep_audio_egress>(released)]()
                      {
                          if (const auto self = weak.lock())
                          {
                              self->finish();
                          }
                      });
}

}    // namespace media_server
