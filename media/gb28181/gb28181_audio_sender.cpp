#include <array>
#include <string>
#include <random>
#include <chrono>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>

#include "media/net/media_port_pool.h"
#include "media/gb28181/gb28181_audio_sender.h"

extern "C"
{
#include "rtp.h"
#include "rtp-payload.h"
}

namespace media_server
{
namespace
{
constexpr auto rtcp_interval = std::chrono::seconds{5};
constexpr auto samples_per_packet = 160U;
}    // namespace

gb28181_audio_sender::gb28181_audio_sender(worker_context& worker, std::function<void(boost::system::error_code)> on_error)
    : worker_(worker),
      error_handler_(std::move(on_error)),
      rtp_transport_(std::make_shared<udp_transport>(worker.io())),
      rtcp_transport_(std::make_shared<udp_transport>(worker.io())),
      rtcp_timer_(worker.io())
{
}

bool gb28181_audio_sender::startup(boost::asio::ip::address bind_address)
{
    local_port_ = media_port_pool::instance().acquire();
    if (!local_port_)
    {
        return false;
    }
    boost::system::error_code error;
    rtp_transport_->startup(bind_address, *local_port_, error);
    if (!error)
    {
        rtcp_transport_->startup(bind_address, static_cast<std::uint16_t>(*local_port_ + 1U), error);
    }
    if (error)
    {
        return false;
    }
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->shutdown(); });
    if (!shutdown_subscription_)
    {
        return false;
    }
    rtp_transport_->set_write_callback([self](boost::system::error_code result, std::size_t) { self->handle_error(result); });
    rtcp_transport_->set_write_callback(
        [self](boost::system::error_code result, std::size_t)
        {
            if (result)
            {
                self->handle_error(result);
                return;
            }
            if (self->rtcp_sender_)
            {
                self->schedule_rtcp();
            }
        });
    return true;
}

bool gb28181_audio_sender::set_peer(boost::asio::ip::udp::endpoint rtp_endpoint, boost::asio::ip::udp::endpoint rtcp_endpoint, std::uint32_t ssrc)
{
    if (!local_port_)
    {
        return false;
    }
    remote_rtp_endpoint_ = std::move(rtp_endpoint);
    remote_rtcp_endpoint_ = std::move(rtcp_endpoint);
    std::random_device random;
    timestamp_ = random();
    rtp_payload_t callbacks{allocate_packet, free_packet, packet_callback};
    packetizer_ = rtp_payload_encode_create(8, "PCMA", static_cast<std::uint16_t>(random()), ssrc, &callbacks, this);
    if (!packetizer_)
    {
        return false;
    }
    rtp_event_t events{};
    rtcp_sender_ = rtp_create(&events, nullptr, ssrc, timestamp_, 8000, 8000, 1);
    if (!rtcp_sender_)
    {
        return false;
    }
    const auto cname = std::to_string(ssrc) + "@media_server";
    static_cast<void>(rtp_set_info(rtcp_sender_, cname.c_str(), "media_server"));
    schedule_rtcp();
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->read_rtcp(yield); });
    return true;
}

bool gb28181_audio_sender::input_audio(std::span<const std::uint8_t> samples)
{
    if (!packetizer_ || !rtcp_sender_ || samples.size() != samples_per_packet)
    {
        return false;
    }
    const auto timestamp = timestamp_;
    timestamp_ += samples_per_packet;
    return rtp_payload_encode_input(packetizer_, samples.data(), static_cast<int>(samples.size()), timestamp) == 0;
}

void gb28181_audio_sender::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_audio_sender::read_rtcp(boost::asio::yield_context yield)
{
    std::array<std::uint8_t, 1500> buffer{};
    while (rtcp_sender_)
    {
        boost::asio::ip::udp::endpoint peer;
        boost::system::error_code error;
        const auto bytes = rtcp_transport_->read(buffer, peer, yield, error);
        if (!rtcp_sender_ || yield.cancelled() != boost::asio::cancellation_type::none)
        {
            return;
        }
        if (error)
        {
            handle_error(error);
            return;
        }
        if (peer != remote_rtcp_endpoint_)
        {
            continue;
        }
        auto packet = std::span<const std::uint8_t>{buffer.data(), bytes};
        bool received_bye{};
        while (!packet.empty())
        {
            if (packet.size() < 4 || (packet[0] >> 6U) != 2U)
            {
                break;
            }
            const auto words = (static_cast<std::size_t>(packet[2]) << 8U) | packet[3];
            const auto size = (words + 1U) * 4U;
            if (size > packet.size())
            {
                break;
            }
            auto member = packet.first(size);
            if ((member[0] & 0x20U) != 0)
            {
                const auto padding = member.back();
                if (size != packet.size() || padding == 0 || padding > size - 4U)
                {
                    break;
                }
                member = member.first(size - padding);
            }
            if (member[1] == RTCP_BYE)
            {
                const auto sources = member[0] & 0x1fU;
                const auto reason_offset = 4U + 4U * sources;
                if (reason_offset > member.size())
                {
                    break;
                }
                if (reason_offset < member.size())
                {
                    const auto reason_end = reason_offset + 1U + member[reason_offset];
                    if (reason_end > member.size() || member.size() - reason_end > 3U)
                    {
                        break;
                    }
                }
                received_bye = received_bye || sources != 0;
            }
            packet = packet.subspan(size);
        }
        if (packet.empty() && received_bye)
        {
            handle_error(boost::asio::error::connection_reset);
            return;
        }
    }
}

void gb28181_audio_sender::schedule_rtcp()
{
    rtcp_timer_.expires_after(rtcp_interval);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || !self->rtcp_sender_)
            {
                return;
            }
            std::array<std::uint8_t, 1500> buffer{};
            const auto bytes = rtp_rtcp_report(self->rtcp_sender_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes <= 0 || !self->rtcp_transport_->write(std::span{buffer.data(), static_cast<std::size_t>(bytes)}, self->remote_rtcp_endpoint_))
            {
                self->schedule_rtcp();
            }
        });
}

void gb28181_audio_sender::handle_error(boost::system::error_code error)
{
    if (!error || !local_port_)
    {
        return;
    }
    auto handler = std::move(error_handler_);
    shutdown();
    if (handler)
    {
        handler(error);
    }
}

void gb28181_audio_sender::safe_shutdown()
{
    error_handler_ = {};
    shutdown_subscription_.reset();
    rtcp_timer_.cancel();
    rtp_transport_->shutdown();
    rtcp_transport_->shutdown();
    if (packetizer_)
    {
        rtp_payload_encode_destroy(packetizer_);
        packetizer_ = nullptr;
    }
    if (rtcp_sender_)
    {
        rtp_destroy(rtcp_sender_);
        rtcp_sender_ = nullptr;
    }
    if (local_port_)
    {
        media_port_pool::instance().release(*local_port_);
        local_port_.reset();
    }
}

void* gb28181_audio_sender::allocate_packet(void* param, int bytes)
{
    auto& self = *static_cast<gb28181_audio_sender*>(param);
    return bytes > 0 && static_cast<std::size_t>(bytes) <= self.packet_buffer_.size() ? self.packet_buffer_.data() : nullptr;
}

void gb28181_audio_sender::free_packet(void*, void*) {}

int gb28181_audio_sender::packet_callback(void* param, const void* data, int bytes, std::uint32_t, int)
{
    auto& self = *static_cast<gb28181_audio_sender*>(param);
    const auto packet = std::span{static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes)};
    if (!self.rtp_transport_->write(packet, self.remote_rtp_endpoint_))
    {
        return -1;
    }
    return rtp_onsend(self.rtcp_sender_, data, bytes);
}

}    // namespace media_server
