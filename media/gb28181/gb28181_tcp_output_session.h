#ifndef MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/asio/any_io_executor.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/net/tcp_socket_source.h"

namespace media_server
{

class gb28181_output_media;
class tcp_connection;

class gb28181_tcp_output_session final : public stream_session, public std::enable_shared_from_this<gb28181_tcp_output_session>
{
   public:
    gb28181_tcp_output_session(boost::asio::any_io_executor executor,
                               std::shared_ptr<tcp_socket_source> socket_source,
                               std::weak_ptr<media_stream> stream,
                               std::string stream_name,
                               std::string output_id,
                               std::uint8_t payload_type,
                               std::uint32_t ssrc);

    [[nodiscard]] bool startup();
    void shutdown() override;

   private:
    void on_socket(boost::system::error_code error, boost::asio::ip::tcp::socket socket);
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<tcp_socket_source> socket_source_;
    std::weak_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string output_id_;
    std::uint8_t payload_type_{};
    std::uint32_t ssrc_{};
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<gb28181_output_media> media_;
    std::atomic_bool shutdown_requested_{};
    bool closed_{};
};

}    // namespace media_server

#endif
