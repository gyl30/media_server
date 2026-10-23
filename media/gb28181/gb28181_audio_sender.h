#ifndef MEDIA_GB28181_GB28181_AUDIO_SENDER_H
#define MEDIA_GB28181_GB28181_AUDIO_SENDER_H

#include <span>
#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <functional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include "media/net/udp_transport.h"
#include "media/net/worker_context.h"

namespace media_server
{

class gb28181_audio_sender final : public std::enable_shared_from_this<gb28181_audio_sender>
{
   public:
    gb28181_audio_sender(worker_context& worker, std::function<void(boost::system::error_code)> on_error);

   public:
    [[nodiscard]] bool startup(boost::asio::ip::address bind_address);
    [[nodiscard]] bool set_peer(boost::asio::ip::udp::endpoint rtp_endpoint, boost::asio::ip::udp::endpoint rtcp_endpoint, std::uint32_t ssrc);
    [[nodiscard]] bool input_audio(std::span<const std::uint8_t> samples);
    [[nodiscard]] std::optional<std::uint16_t> local_port() const noexcept { return local_port_; }
    void shutdown();

   private:
    static void* allocate_packet(void* param, int bytes);
    static void free_packet(void* param, void* packet);
    static int packet_callback(void* param, const void* data, int bytes, std::uint32_t timestamp, int flags);

   private:
    void read_rtcp(boost::asio::yield_context yield);
    void schedule_rtcp();
    void handle_error(boost::system::error_code error);
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::function<void(boost::system::error_code)> error_handler_;
    std::shared_ptr<udp_transport> rtp_transport_;
    std::shared_ptr<udp_transport> rtcp_transport_;
    boost::asio::steady_timer rtcp_timer_;
    std::optional<std::uint16_t> local_port_;
    boost::asio::ip::udp::endpoint remote_rtp_endpoint_;
    boost::asio::ip::udp::endpoint remote_rtcp_endpoint_;
    void* packetizer_{};
    void* rtcp_sender_{};
    std::uint32_t timestamp_{};
    std::array<std::uint8_t, 12 + 160> packet_buffer_{};
    worker_context::shutdown_subscription shutdown_subscription_;
};

}    // namespace media_server

#endif
