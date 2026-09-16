#ifndef MEDIA_HTTP_SIGNALING_CLIENT_H
#define MEDIA_HTTP_SIGNALING_CLIENT_H

#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <boost/asio/spawn.hpp>

#include "media/core/runtime_event.h"

namespace media_server
{

enum class signaling_result_kind
{
    accepted,
    rejected,
    temporary_failure,
    network_error,
};

struct signaling_request_result
{
    signaling_result_kind kind{signaling_result_kind::network_error};
    unsigned int status{};
    std::string error;
};

struct signaling_client_options
{
    std::string signaling_url;
    std::string server_id;
    std::string instance_id;
    std::string control_url;
    std::string media_ip;
    std::uint16_t rtmp_port{};
    std::uint16_t rtsp_port{};
    std::uint16_t http_port{};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{5}};
    std::chrono::milliseconds request_timeout{std::chrono::seconds{3}};
};

class signaling_client
{
   public:
    [[nodiscard]] static signaling_client& instance();

    void configure(signaling_client_options options);

    signaling_request_result register_once(boost::asio::yield_context& yield) const;
    signaling_request_result heartbeat_once(boost::asio::yield_context& yield) const;
    signaling_request_result claim_publish(std::string_view stream_id,
                                           std::string_view protocol,
                                           std::string_view stream_name,
                                           boost::asio::yield_context& yield) const;
    void report(runtime_event event);
    void run(boost::asio::yield_context& yield, std::function<void()> fenced_handler);

   private:
    struct request_state;

    signaling_request_result request(std::string_view target,
                                     std::string body,
                                     std::string host,
                                     std::string port,
                                     std::chrono::milliseconds timeout,
                                     boost::asio::yield_context& yield) const;

   private:
    static constexpr std::size_t max_pending_events = 500U;
    std::mutex event_mutex_;
    std::vector<runtime_event> pending_events_;
    signaling_client_options options_;
    std::string host_;
    std::string port_;
};

}    // namespace media_server

#endif
