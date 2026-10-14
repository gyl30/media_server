#ifndef MEDIA_WEBRTC_WHEP_PULL_SESSION_H
#define MEDIA_WEBRTC_WHEP_PULL_SESSION_H

#include <span>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/idle_timer.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/net/udp_transport.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/srtp_transport.h"
#include "media/core/session_registry.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/whip_media_receiver.h"

namespace media_server
{
class worker_context;

// url 为外部 WHEP 端点（http://），token 非空时作为 Bearer 认证；local_address 为本端 UDP 绑定与 candidate 地址。
struct whep_pull_config
{
    std::string stream_id;
    std::string url;
    std::string token;
    boost::asio::ip::address local_address;
};

// 平台作为 WHEP 客户端从外部端点接收媒体：HTTP 提交 offer，作为 ICE controlling 方向应答中的 candidate 做连通性检查，
// 完成 DTLS（按应答的 setup 决定角色）与 SRTP 后，经 whip_media_receiver 解包、Opus 转 AAC 并注册媒体源。
// 只支持应答 candidate 可直接到达的网络（无 STUN/TURN 服务器、不做 Trickle ICE 与 ICE restart）。
class whep_pull_session final : public session, public std::enable_shared_from_this<whep_pull_session>
{
   public:
    whep_pull_session(worker_context& worker, whep_pull_config config);
    ~whep_pull_session();

   public:
    void startup();
    void shutdown() override;

   private:
    void run(boost::asio::yield_context yield);
    bool negotiate(boost::asio::yield_context yield);
    void run_udp(boost::asio::yield_context yield);

   private:
    void handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    void handle_dtls(std::span<const std::uint8_t> packet);
    void handle_srtp(std::span<const std::uint8_t> packet);
    void send_checks();
    void schedule_checks();
    void schedule_dtls_timeout();
    void send_udp(std::vector<std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint);
    bool aborted(const boost::asio::yield_context& yield) const;

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    whep_pull_config config_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer check_timer_;
    boost::asio::steady_timer dtls_timer_;
    boost::asio::steady_timer establishment_timer_;
    idle_timer idle_timer_;
    std::shared_ptr<udp_transport> udp_transport_;
    std::shared_ptr<dtls_certificate> certificate_;
    std::unique_ptr<dtls_transport> dtls_;
    std::unique_ptr<srtp_transport> srtp_;
    std::unique_ptr<whip_media_receiver> media_receiver_;
    std::optional<whep_pull_answer> answer_;
    std::optional<boost::asio::ip::udp::endpoint> selected_;
    std::string resource_url_;
    std::uint16_t local_port_{};
    std::string ice_ufrag_;
    std::string ice_pwd_;
    std::uint64_t tie_breaker_{};
    std::array<std::uint8_t, 8> transaction_prefix_{};
    std::uint32_t check_sequence_{};
    struct pending_check
    {
        std::array<std::uint8_t, 12> transaction;
        boost::asio::ip::udp::endpoint endpoint;
        std::chrono::steady_clock::time_point sent;
    };
    std::vector<pending_check> pending_checks_;
    std::chrono::steady_clock::time_point consent_received_{};
};

}    // namespace media_server

#endif
