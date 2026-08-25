#ifndef MEDIA_GB28181_GB28181_TCP_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_SESSION_H

#include <span>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/asio/any_io_executor.hpp>

#include "media/net/tcp_connection.h"
#include "media/net/tcp_socket_source.h"
#include "media/gb28181/gb28181_input_media.h"
#include "media/gb28181/gb28181_session.h"

namespace media_server
{

class gb28181_tcp_session final : public gb28181_session, public std::enable_shared_from_this<gb28181_tcp_session>
{
   public:
    gb28181_tcp_session(boost::asio::any_io_executor executor,
                        std::shared_ptr<tcp_socket_source> socket_source,
                        std::string stream_name,
                        std::uint8_t payload_type,
                        std::uint32_t expected_ssrc);

    [[nodiscard]] bool startup();
    void shutdown() override;

    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    void on_socket(boost::system::error_code error, boost::asio::ip::tcp::socket socket);
    void on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<tcp_socket_source> socket_source_;
    std::string stream_name_;
    gb28181_input_media media_;
    std::shared_ptr<tcp_connection> connection_;
    std::vector<std::uint8_t> input_buffer_;
    std::atomic_bool shutdown_requested_{};
    bool closed_{};
};

}    // namespace media_server

#endif
