#ifndef MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H
#define MEDIA_GB28181_GB28181_TCP_RECEIVER_SESSION_H

#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/tcp_listener.h"
#include "media/core/runtime_event.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/net/tcp_yield_transport.h"
#include "media/gb28181/gb28181_rtp_receiver.h"

namespace media_server
{
class worker_context;

class gb28181_tcp_receiver_session final : public stream_session, public std::enable_shared_from_this<gb28181_tcp_receiver_session>
{
   public:
    gb28181_tcp_receiver_session(worker_context& worker,
                                 std::string stream_id,
                                 std::string stream_name,
                                 gb28181_transport_config config,
                                 boost::asio::ip::address bind_address,
                                 std::chrono::milliseconds establishment_timeout,
                                 runtime_event_emitter_ptr runtime_events = {});

    [[nodiscard]] bool startup();
    void shutdown(runtime_end_reason reason = runtime_end_reason::requested, std::string error = {}) override;
    [[nodiscard]] std::string_view stream_id() const noexcept override;

   private:
    void run(boost::asio::yield_context yield);
    void safe_shutdown();
    void emit_starting();
    void emit_streaming();
    void emit_stopped();

   private:
    worker_context& worker_;
    std::string stream_id_;
    gb28181_transport_config config_;
    boost::asio::ip::address bind_address_;
    gb28181_rtp_receiver receiver_;
    std::chrono::milliseconds establishment_timeout_{};
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<tcp_listener> listener_;
    std::unique_ptr<tcp_yield_transport> transport_;
    runtime_event_emitter_ptr runtime_events_;
    runtime_end_reason end_reason_{runtime_end_reason::requested};
    std::string end_error_;
    bool started_{};
    bool runtime_started_{};
    bool runtime_streaming_{};
    bool ending_{};
    bool closed_{};
};

}    // namespace media_server

#endif
