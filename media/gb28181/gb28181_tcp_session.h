#ifndef MEDIA_GB28181_GB28181_TCP_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_SESSION_H

#include <chrono>
#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/tcp_listener.h"
#include "media/net/tcp_connection.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_input_media.h"

namespace media_server
{
class worker_context;

class gb28181_tcp_session final : public stream_session, public std::enable_shared_from_this<gb28181_tcp_session>
{
   public:
    gb28181_tcp_session(worker_context& worker,
                        std::string stream_name,
                        gb28181_description description,
                        std::chrono::milliseconds establishment_timeout);

    [[nodiscard]] bool startup();
    void shutdown() override;

    [[nodiscard]] const std::string& stream_name() const noexcept;

   private:
    void run(boost::asio::yield_context yield);
    void on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();

    worker_context& worker_;
    std::string stream_name_;
    gb28181_description description_;
    gb28181_input_media media_;
    std::chrono::milliseconds establishment_timeout_{};
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::shared_ptr<tcp_connection> connection_;
    std::vector<std::uint8_t> input_buffer_;
    bool closed_{};
};

}    // namespace media_server

#endif
