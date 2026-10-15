#ifndef MEDIA_GB28181_GB28181_PLAYBACK_SESSION_H
#define MEDIA_GB28181_GB28181_PLAYBACK_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/beast/core.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/worker_context.h"
#include "media/core/session_registry.h"

namespace media_server
{

// 有限录像的独占原始 PS 通道，不注册实时 media_stream。
class gb28181_playback_session final : public session, public std::enable_shared_from_this<gb28181_playback_session>
{
   public:
    gb28181_playback_session(worker_context& worker, std::string stream_id, std::uint8_t payload_type, std::uint32_t ssrc);

   public:
    [[nodiscard]] std::optional<std::uint16_t> startup(boost::asio::ip::address bind_address);
    void attach(boost::asio::ip::tcp::socket socket, unsigned http_version);
    void update_ssrc(std::uint32_t ssrc);
    void shutdown() override;

   private:
    void run(boost::asio::ip::tcp::socket socket, unsigned http_version, boost::asio::yield_context yield);
    void watch_consumer(boost::asio::yield_context yield);
    void safe_shutdown();

   private:
    worker_context& worker_;
    const std::string stream_id_;
    const std::uint8_t payload_type_;
    std::uint32_t ssrc_;
    std::optional<std::uint16_t> local_port_;
    boost::asio::ip::tcp::acceptor listener_;
    boost::asio::ip::tcp::socket input_;
    std::unique_ptr<boost::beast::tcp_stream> consumer_;
    worker_context::shutdown_subscription shutdown_subscription_;
};

}    // namespace media_server

#endif
