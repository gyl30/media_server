#ifndef MEDIA_WEBRTC_WHEP_SESSION_H
#define MEDIA_WEBRTC_WHEP_SESSION_H

#include "media/core/media_reader.h"
#include "media/core/media_stream.h"
#include "media/net/udp_socket.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/rtcp_receiver.h"
#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/webrtc_output.h"
#include "media/webrtc/webrtc_sdp.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace media_server
{

struct whep_rtcp_stats
{
    std::size_t receiver_reports{};
    std::size_t plis{};
};

struct whep_session_timeouts
{
    std::chrono::milliseconds establishment{15'000};
    std::chrono::milliseconds ice_activity{30'000};
};

enum class whep_session_startup_error
{
    none,
    invalid_offer,
    stream_not_ready,
    internal_error,
};

class whep_session final : public media_reader, public std::enable_shared_from_this<whep_session>
{
   public:
    whep_session(boost::asio::any_io_executor executor,
                 std::shared_ptr<media_stream> stream,
                 boost::asio::ip::address advertised_address,
                 std::shared_ptr<dtls_certificate> certificate,
                 whep_session_timeouts timeouts = {});

    [[nodiscard]] whep_session_startup_error startup(webrtc_offer offer);
    void shutdown();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& answer_sdp() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] bool ice_connected() const noexcept;
    [[nodiscard]] bool dtls_connected() const noexcept;
    [[nodiscard]] bool srtp_started() const noexcept;
    [[nodiscard]] const whep_rtcp_stats& rtcp_stats() const noexcept;

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    void safe_shutdown();
    void on_udp_read(boost::system::error_code error,
                     std::span<const std::uint8_t> packet,
                     const boost::asio::ip::udp::endpoint& endpoint);
    void on_udp_write(boost::system::error_code error,
                      std::size_t bytes,
                      const boost::asio::ip::udp::endpoint& endpoint);
    void handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_dtls(std::span<const std::uint8_t> packet);
    void handle_srtp(std::span<const std::uint8_t> packet);
    bool startup_media();
    bool apply_tracks(const media_track_snapshot_ptr& tracks);
    bool start_media_read();
    void send_dtls(std::span<const std::uint8_t> packet);
    void send_rtp(std::span<const std::uint8_t> packet);
    void send_rtcp(std::span<const std::uint8_t> packet);
    void send_udp(std::vector<std::uint8_t> packet);
    void schedule_dtls_timeout();
    void handle_dtls_timeout();
    void startup_establishment_timeout();
    void refresh_ice_activity_timeout();

    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    whep_session_timeouts timeouts_;
    media_reader_handle reader_;
    std::map<track_id, std::uint64_t> track_versions_;
    std::vector<media_track> pending_tracks_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    rtcp_receiver rtcp_receiver_;
    whep_rtcp_stats rtcp_stats_;
    std::shared_ptr<webrtc_output> output_;
    boost::asio::any_io_executor executor_;
    std::shared_ptr<udp_socket> udp_socket_;
    boost::asio::steady_timer dtls_timer_;
    boost::asio::steady_timer establishment_timer_;
    boost::asio::steady_timer ice_activity_timer_;
    std::optional<boost::asio::ip::udp::endpoint> remote_endpoint_;
    std::string id_;
    std::string ice_ufrag_;
    std::string ice_pwd_;
    std::string remote_ice_ufrag_;
    std::string answer_sdp_;
    std::optional<codec_id> video_codec_;
    std::optional<int> video_payload_type_;
    std::optional<int> audio_payload_type_;
    std::optional<int> audio_channel_count_;
    std::uint16_t local_port_{};
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool tracks_ready_{};
    bool started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
