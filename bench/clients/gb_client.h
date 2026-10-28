#ifndef MEDIA_SERVER_BENCH_CLIENTS_GB_CLIENT_H
#define MEDIA_SERVER_BENCH_CLIENTS_GB_CLIENT_H

#include <span>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "bench/packet_health.h"

namespace media_server::bench
{

enum class gb_transport
{
    udp,
    tcp_active,
    tcp_passive,
};

enum class gb_peer
{
    quiet,
    rtcp,
    rtcp_rr,
    rr,
    rtp_noise,
};

struct gb_peer_counters
{
    std::uint64_t sender_reports{};
    std::uint64_t invalid_rtcp{};
    std::uint64_t receiver_reports{};
    std::uint64_t noise_datagrams{};
};

struct gb_client_options
{
    gb_transport transport{gb_transport::udp};
    std::string control_token;
    boost::asio::ip::address bind_address;
    std::uint16_t listen_port{};
    std::uint8_t payload_type{96};
    gb_peer peer{gb_peer::quiet};
};

struct gb_sender_key
{
    std::string stream_id;
    std::string sender_id;
};

class gb_client final
{
   public:
    gb_client(boost::asio::io_context& io, const std::string& path);

    static void configure(gb_client_options value);
    static const std::vector<gb_sender_key>& created_senders();
    static const std::vector<std::uint64_t>& create_milliseconds();
    static const std::vector<std::uint64_t>& delete_milliseconds();
    static const gb_peer_counters& peer_counters();
    static boost::asio::awaitable<bool> delete_sender(std::string host, std::uint16_t port, gb_sender_key key);

   public:
    boost::asio::awaitable<boost::system::error_code> play(std::string host, std::uint16_t port, bool expect_audio);
    boost::asio::awaitable<boost::system::error_code> consume_one();
    void cancel() noexcept;
    [[nodiscard]] bool handshake_complete() const noexcept { return handshake_complete_; }

    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return video_bytes_; }
    [[nodiscard]] std::uint64_t received_messages() const noexcept { return video_frames_; }
    [[nodiscard]] std::uint64_t received_audio_bytes() const noexcept { return audio_bytes_; }
    [[nodiscard]] std::uint64_t received_audio_messages() const noexcept { return audio_frames_; }
    [[nodiscard]] std::uint64_t video_media_ms() const noexcept;

   private:
    boost::asio::awaitable<boost::system::error_code> create_sender(std::string host, std::uint16_t port, std::uint16_t media_port);
    boost::asio::awaitable<boost::system::error_code> receive();
    bool on_packet(std::span<const std::uint8_t> packet);
    boost::asio::awaitable<void> send_feedback();
    boost::asio::awaitable<void> receive_rtcp();

   private:
    boost::asio::ip::udp::socket udp_socket_;
    boost::asio::ip::udp::socket rtcp_socket_;
    boost::asio::steady_timer feedback_timer_;
    boost::asio::ip::udp::endpoint media_source_;
    boost::asio::ip::tcp::socket tcp_socket_;
    boost::asio::ip::tcp::acceptor acceptor_;
    gb_sender_key key_;
    std::uint32_t ssrc_{};
    std::vector<std::uint8_t> buffer_;
    std::size_t buffered_{};
    std::optional<std::uint16_t> sequence_;
    std::optional<std::uint32_t> frame_timestamp_;
    bool video_frame_{};
    timestamp_health video_timestamps_;
    timestamp_health audio_timestamps_;
    std::uint64_t video_bytes_{};
    std::uint64_t video_frames_{};
    std::uint64_t audio_bytes_{};
    std::uint64_t audio_frames_{};
    bool handshake_complete_{};
};

}    // namespace media_server::bench

#endif
