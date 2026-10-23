#ifndef MEDIA_GB28181_GB28181_AUDIO_SESSION_H
#define MEDIA_GB28181_GB28181_AUDIO_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <optional>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket.hpp>

#include "media/net/idle_timer.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_audio_sender.h"

namespace media_server
{

class gb28181_audio_session final : public session, public std::enable_shared_from_this<gb28181_audio_session>
{
   public:
    gb28181_audio_session(worker_context& worker, std::string id);

   public:
    [[nodiscard]] std::optional<std::uint16_t> startup(boost::asio::ip::address bind_address);
    [[nodiscard]] bool configure(boost::asio::ip::udp::endpoint rtp, boost::asio::ip::udp::endpoint rtcp, std::uint32_t ssrc, std::string token);
    void attach(boost::asio::ip::tcp::socket socket, boost::beast::http::request<boost::beast::http::string_body> request, std::string token);
    void keep_alive();
    [[nodiscard]] worker_context& worker() noexcept { return worker_; }
    void shutdown() override;

   private:
    void run(boost::asio::ip::tcp::socket socket,
             boost::beast::http::request<boost::beast::http::string_body> request,
             const std::string& token,
             boost::asio::yield_context yield);
    void safe_shutdown();

   private:
    worker_context& worker_;
    const std::string id_;
    std::string upload_token_;
    std::shared_ptr<gb28181_audio_sender> sender_;
    std::unique_ptr<boost::beast::websocket::stream<boost::beast::tcp_stream>> upload_;
    idle_timer control_lease_;
    idle_timer audio_idle_;
    worker_context::shutdown_subscription shutdown_subscription_;
};

}    // namespace media_server

#endif
