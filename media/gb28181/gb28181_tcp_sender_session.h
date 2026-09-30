#ifndef MEDIA_GB28181_GB28181_TCP_SENDER_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_SENDER_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/address.hpp>

#include "media/net/tcp_listener.h"
#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/net/tcp_transport.h"

namespace media_server
{
class worker_context;

class gb28181_rtp_sender;

class gb28181_tcp_sender_session final : public session, public std::enable_shared_from_this<gb28181_tcp_sender_session>
{
   public:
    gb28181_tcp_sender_session(worker_context& worker, std::shared_ptr<media_stream> stream, std::string sender_id);

   public:
    void startup(boost::asio::ip::tcp::endpoint remote_endpoint, std::uint8_t payload_type, std::uint32_t ssrc);
    [[nodiscard]] bool startup(boost::asio::ip::address bind_address, std::uint16_t listen_port, std::uint8_t payload_type, std::uint32_t ssrc);
    void shutdown();

   private:
    void run(std::optional<boost::asio::ip::tcp::endpoint> remote_endpoint,
             std::uint8_t payload_type,
             std::uint32_t ssrc,
             boost::asio::yield_context yield);
    void run_read(boost::asio::yield_context yield);
    void send_packet(std::vector<std::uint8_t> packet);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<media_stream> stream_;
    std::string sender_id_;
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::shared_ptr<tcp_transport> transport_;
    std::shared_ptr<gb28181_rtp_sender> sender_;
};

}    // namespace media_server

#endif
