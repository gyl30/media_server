#ifndef MEDIA_GB28181_GB28181_TCP_SENDER_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_SENDER_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_listener.h"
#include "media/core/media_stream.h"
#include "media/net/tcp_write_queue.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/net/tcp_yield_transport.h"

namespace media_server
{
class worker_context;

class gb28181_rtp_sender;

class gb28181_tcp_sender_session final : public stream_session, public std::enable_shared_from_this<gb28181_tcp_sender_session>
{
   public:
    gb28181_tcp_sender_session(worker_context& worker,
                               std::string stream_id,
                               std::shared_ptr<media_stream> stream,
                               std::string sender_id,
                               gb28181_transport_config config,
                               boost::asio::ip::address bind_address,
                               std::chrono::milliseconds establishment_timeout,
                               std::size_t max_write_queue_bytes = 1024U * 1024U);

    [[nodiscard]] bool startup();
    void shutdown() override;
    [[nodiscard]] std::string_view stream_id() const noexcept override;

   private:
    void run(boost::asio::yield_context yield);
    void run_write(boost::asio::yield_context yield);
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::string stream_id_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string sender_id_;
    gb28181_transport_config config_;
    boost::asio::ip::address bind_address_;
    std::chrono::milliseconds establishment_timeout_{};
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::unique_ptr<tcp_yield_transport> transport_;
    tcp_write_queue write_queue_;
    std::shared_ptr<gb28181_rtp_sender> sender_;
    bool started_{};
    bool media_started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
