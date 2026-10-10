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
                                       packet_handler on_packet,
                                       end_handler handle_source_end)
    : worker_(worker), stream_(std::move(stream)), packet_handler_(std::move(on_packet)), end_handler_(std::move(handle_source_end))
{
}

bool gb28181_rtp_sender::startup(std::uint8_t payload_type, std::uint32_t ssrc)
{
    std::random_device device;
    timestamp_base_ = device();
    rtp_payload_t callbacks{allocate_packet, free_packet, packet_callback};
    packetizer_ = rtp_payload_encode_create(payload_type, "PS", static_cast<std::uint16_t>(device()), ssrc, &callbacks, this);
    if (packetizer_ == nullptr)
    {
        return false;
    }

    for (const auto& track : stream_->tracks())
    {
        if (track.kind == media_kind::video)
        {
            waiting_video_track_ = track.id;
            break;
        }
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
                                            [self, source, output = std::move(output)]()
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
                                                output->add_sink(self);
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
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_rtp_sender::on_ps_frame(const mpeg_ps_frame& frame)
{
    if (shutdown_requested_.load(std::memory_order_acquire))
    {
        return;
    }
    if (!frame.payload)
    {
        return;
    }

    const bool starts_media = waiting_video_track_.has_value();
    if (starts_media && (frame.track != *waiting_video_track_ || !frame.key_frame))
    {
        return;
    }

    const auto media_timestamp = frame.media_timestamp;
    if (starts_media)
    {
        first_media_timestamp_ = media_timestamp;
    }
    const auto timestamp = timestamp_base_ + media_timestamp - first_media_timestamp_;
    const auto result = rtp_payload_encode_input(packetizer_, frame.payload->data(), static_cast<int>(frame.payload->size()), timestamp);
    if (result < 0)
    {
        spdlog::error("gb28181 sender mux failed stream {} result {}", stream_->stream_id(), result);
        shutdown();
        end_handler_();
        return;
    }
    if (starts_media)
    {
        waiting_video_track_.reset();
    }
}

void gb28181_rtp_sender::on_end()
{
    if (!shutdown_requested_.load(std::memory_order_acquire))
    {
        end_handler_();
    }
}

void gb28181_rtp_sender::safe_shutdown()
{
    packet_handler_ = {};
    end_handler_ = {};
    stream_.reset();
    ps_output_.reset();
    if (packetizer_)
    {
        rtp_payload_encode_destroy(packetizer_);
        packetizer_ = nullptr;
    }
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
    const auto* begin = static_cast<const std::uint8_t*>(data);
    self.packet_handler_(std::vector<std::uint8_t>(begin, begin + bytes));
    return 0;
}

}    // namespace media_server
