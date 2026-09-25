#include <random>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_rtp_sender.h"

extern "C"
{
#include "rtp-payload.h"
#include "rtp-profile.h"
}

namespace media_server
{

gb28181_rtp_sender::gb28181_rtp_sender(worker_context& worker,
                                       std::shared_ptr<media_stream> stream,
                                       std::uint8_t payload_type,
                                       std::uint32_t ssrc,
                                       packet_handler on_packet,
                                       end_handler on_end,
                                       failure_handler on_failure)
    : worker_(worker),
      stream_(std::move(stream)),
      payload_type_(payload_type),
      ssrc_(ssrc),
      packet_handler_(std::move(on_packet)),
      end_handler_(std::move(on_end)),
      failure_handler_(std::move(on_failure))
{
}

bool gb28181_rtp_sender::supported_tracks(const std::vector<media_track>& tracks) { return mpeg_ps_output::supported_tracks(tracks); }

bool gb28181_rtp_sender::startup()
{
    if (shutdown_requested_.load(std::memory_order_acquire) || !stream_ || packetizer_ || !packet_handler_ ||
        !supported_tracks(stream_->tracks()) || !create_packetizer())
    {
        return false;
    }
    const auto self = shared_from_this();
    const auto source = stream_;
    boost::asio::post(source->worker().io(),
                      [self, source]()
                      {
                          if (self->shutdown_requested_.load(std::memory_order_acquire))
                          {
                              return;
                          }
                          auto output = source->ps_output();
                          boost::asio::post(self->worker_.io(),
                                            [self, output = std::move(output)]()
                                            {
                                                if (self->shutdown_requested_.load(std::memory_order_acquire))
                                                {
                                                    return;
                                                }
                                                if (!output)
                                                {
                                                    self->on_end();
                                                    return;
                                                }
                                                self->ps_output_ = output;
                                                output->stream()->add_reader(self, self->worker_);
                                            });
                      });
    return true;
}

void gb28181_rtp_sender::shutdown()
{
    if (shutdown_requested_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    reader_handle().remove();
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_rtp_sender::on_tracks(media_track_snapshot_ptr tracks)
{
    if (shutdown_requested_.load(std::memory_order_acquire) || !packet_handler_)
    {
        return;
    }
    apply_tracks(tracks);
    reader_handle().async_read(reader_cursor_);
}

void gb28181_rtp_sender::on_read(media_read_batch_t<mpeg_ps_frame> batch)
{
    if (shutdown_requested_.load(std::memory_order_acquire) || !packet_handler_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    apply_tracks(batch.tracks);

    for (const auto& entry : batch.entries)
    {
        const auto state = track_states_.find(entry.frame.track);
        if (state == track_states_.end() || state->second.config_version != entry.config_version || !entry.frame.payload)
        {
            continue;
        }

        const bool starts_media = waiting_for_key_frame_;
        if (starts_media)
        {
            if (state->second.kind != media_kind::video || !entry.frame.key_frame)
            {
                continue;
            }
        }

        const auto media_timestamp = entry.frame.media_timestamp;
        if (!first_media_timestamp_)
        {
            first_media_timestamp_ = media_timestamp;
        }
        const auto timestamp = timestamp_base_ + media_timestamp - *first_media_timestamp_;
        const auto result =
            rtp_payload_encode_input(packetizer_, entry.frame.payload->data(), static_cast<int>(entry.frame.payload->size()), timestamp);
        if (result < 0)
        {
            spdlog::error("gb28181 sender mux failed stream {} result {}", stream_->name(), result);
            reader_handle().remove();
            if (failure_handler_)
            {
                failure_handler_();
            }
            else if (end_handler_)
            {
                end_handler_();
            }
            else
            {
                shutdown();
            }
            return;
        }
        if (starts_media)
        {
            waiting_for_key_frame_ = false;
        }
    }

    if (packet_handler_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void gb28181_rtp_sender::on_end()
{
    if (ps_output_ && ps_output_->failed() && failure_handler_)
    {
        failure_handler_();
    }
    else if (end_handler_)
    {
        end_handler_();
    }
}

void gb28181_rtp_sender::safe_shutdown()
{
    if (!stream_)
    {
        return;
    }
    packet_handler_ = {};
    end_handler_ = {};
    failure_handler_ = {};
    reader_handle().remove();
    reader_cursor_.reset();
    track_revision_ = 0;
    track_states_.clear();
    waiting_for_key_frame_ = true;
    stream_.reset();
    ps_output_.reset();
    if (packetizer_)
    {
        rtp_payload_encode_destroy(packetizer_);
        packetizer_ = nullptr;
    }
}

bool gb28181_rtp_sender::create_packetizer()
{
    std::random_device device;
    timestamp_base_ = device();
    rtp_payload_t callbacks{allocate_packet, free_packet, packet_callback};
    packetizer_ = rtp_payload_encode_create(payload_type_, "PS", static_cast<std::uint16_t>(device()), ssrc_, &callbacks, this);
    return packetizer_ != nullptr;
}

void gb28181_rtp_sender::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return;
    }

    bool video_changed = false;
    for (const auto& track : tracks->tracks)
    {
        auto& state = track_states_[track.id];
        state.kind = track.kind;
        if (track.kind == media_kind::video && state.config_version != track.config_version)
        {
            video_changed = true;
        }
        state.config_version = track.config_version;
    }

    track_revision_ = tracks->revision;
    waiting_for_key_frame_ = waiting_for_key_frame_ || video_changed;
}

void* gb28181_rtp_sender::allocate_packet(void* param, int bytes)
{
    auto& self = *static_cast<gb28181_rtp_sender*>(param);
    return bytes > 0 && static_cast<std::size_t>(bytes) <= self.packet_buffer_.size() ? self.packet_buffer_.data() : nullptr;
}

void gb28181_rtp_sender::free_packet(void*, void*) {}

int gb28181_rtp_sender::packet_callback(void* param, const void* data, int bytes, std::uint32_t, int)
{
    auto& self = *static_cast<gb28181_rtp_sender*>(param);
    if (!self.packet_handler_)
    {
        return -1;
    }
    const auto* begin = static_cast<const std::uint8_t*>(data);
    self.packet_handler_(std::vector<std::uint8_t>(begin, begin + bytes));
    return 0;
}

}    // namespace media_server
