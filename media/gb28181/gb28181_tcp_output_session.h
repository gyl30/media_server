#ifndef MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_OUTPUT_SESSION_H

#include "media/core/media_stream.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace media_server
{

class gb28181_output_media;
class tcp_connection;

class gb28181_tcp_output_session final : public std::enable_shared_from_this<gb28181_tcp_output_session>
{
   public:
    gb28181_tcp_output_session(boost::asio::ip::tcp::socket socket,
                               std::shared_ptr<media_stream> stream,
                               std::uint8_t payload_type,
                               std::uint32_t ssrc);

    [[nodiscard]] bool startup();
    void shutdown();

   private:
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::uint8_t payload_type_{};
    std::uint32_t ssrc_{};
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<gb28181_output_media> media_;
    bool closed_{};
};

}    // namespace media_server

#endif
