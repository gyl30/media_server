#ifndef MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H

#include <memory>
#include <functional>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/address.hpp>

#include "media/net/idle_timer.h"
#include "media/net/tcp_listener.h"
#include "media/core/session_registry.h"
#include "media/net/tcp_transport.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_tcp_receiver_session final : public session, public std::enable_shared_from_this<gb28181_tcp_receiver_session>
{
   public:
    gb28181_tcp_receiver_session(worker_context& worker, std::string stream_name, std::uint8_t payload_type, std::uint32_t ssrc);

   public:
    [[nodiscard]] bool startup(boost::asio::ip::tcp::endpoint remote_endpoint);
    [[nodiscard]] bool startup(boost::asio::ip::address bind_address, std::uint16_t listen_port);
    void shutdown();
    // 在接收器 worker 上确认仍在运行且槽位未进入关闭后更新 SSRC，结果通过 done 返回。
    void update_ssrc(std::uint32_t ssrc, std::function<void(bool)> done);

   private:
    void run(std::optional<boost::asio::ip::tcp::endpoint> remote_endpoint, boost::asio::yield_context yield);
    void run_read(boost::asio::yield_context yield);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    gb28181_rtp_receiver receiver_;
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::shared_ptr<tcp_transport> transport_;
    idle_timer idle_timer_;
    bool closed_{};
};

}    // namespace media_server

#endif
