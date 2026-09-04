#ifndef MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_listener.h"
#include "media/gb28181/gb28181_types.h"

namespace media_server
{
class worker_context;

class gb28181_output_media;
class tcp_connection;

class gb28181_tcp_output_session final : public stream_session, public std::enable_shared_from_this<gb28181_tcp_output_session>
{
   public:
    gb28181_tcp_output_session(worker_context& worker,
                               std::weak_ptr<media_stream> stream,
                               std::string stream_name,
                               std::string output_id,
                               gb28181_description description,
                               std::chrono::milliseconds establishment_timeout);

    [[nodiscard]] bool startup();
    void shutdown() override;

   private:
    void run(boost::asio::yield_context yield);
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    worker_context& worker_;
    std::weak_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string output_id_;
    gb28181_description description_;
    std::chrono::milliseconds establishment_timeout_{};
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<gb28181_output_media> media_;
    bool closed_{};
};

}    // namespace media_server

#endif
