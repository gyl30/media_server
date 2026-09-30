#ifndef MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H

#include <memory>
#include <string>
#include <cstdint>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_listener.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/net/tcp_transport.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_tcp_receiver_session final : public session, public std::enable_shared_from_this<gb28181_tcp_receiver_session>
{
   public:
    gb28181_tcp_receiver_session(worker_context& worker,
                                 std::string stream_name,
                                 gb28181_transport_config config,
                                 boost::asio::ip::address bind_address);

   public:
    void startup();
    void shutdown();

   private:
    void run(boost::asio::yield_context yield);
    void run_read(boost::asio::yield_context yield);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    gb28181_transport_config config_;
    boost::asio::ip::address bind_address_;
    gb28181_rtp_receiver receiver_;
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::shared_ptr<tcp_transport> transport_;
};

}    // namespace media_server

#endif
