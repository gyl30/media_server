#ifndef MEDIA_GB28181_GB28181_TCP_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_connection.h"
#include "media/gb28181/gb28181_input_media.h"

namespace media_server
{

class gb28181_tcp_session final : public std::enable_shared_from_this<gb28181_tcp_session>
{
   public:
    gb28181_tcp_session(stream_registry& registry,
                        boost::asio::ip::tcp::socket socket,
                        std::string stream_name,
                        std::uint8_t payload_type,
                        std::uint32_t expected_ssrc);

    [[nodiscard]] bool startup();
    void shutdown();

    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    void on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    gb28181_input_media media_;
    std::shared_ptr<tcp_connection> connection_;
    std::vector<std::uint8_t> input_buffer_;
    bool closed_{};
};

}    // namespace media_server

#endif
