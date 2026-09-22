#ifndef MEDIA_HTTP_SIGNALING_CLIENT_H
#define MEDIA_HTTP_SIGNALING_CLIENT_H

#include <mutex>
#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include <cstddef>
#include <string_view>

#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include "config.h"
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

class signaling_client
{
   public:
    [[nodiscard]] static signaling_client& instance();

   public:
    void configure(const config& cfg,
                   std::string instance_id,
                   std::chrono::milliseconds heartbeat_interval = std::chrono::seconds{5},
                   std::chrono::milliseconds request_timeout = std::chrono::seconds{3});

    signaling_request_result register_once(boost::asio::yield_context& yield) const;
    signaling_request_result heartbeat_once(boost::asio::yield_context& yield) const;
    signaling_request_result claim_publish(std::string_view stream_id,
                                           std::string_view protocol,
                                           std::string_view stream_name,
                                           boost::asio::yield_context& yield) const;
    signaling_request_result claim_play(std::string_view stream_id,
                                        std::string_view protocol,
                                        std::string_view stream_name,
                                        boost::asio::yield_context& yield) const;
    void report(runtime_event event);
    void run(boost::asio::yield_context& yield);

   private:
    signaling_request_result request(std::string_view target,
                                     std::string body,
                                     std::string host,
                                     std::string port,
                                     std::chrono::milliseconds timeout,
                                     boost::asio::yield_context& yield) const;

   private:
    static constexpr std::size_t max_pending_events = 500U;
    static constexpr std::chrono::milliseconds runtime_event_batch_delay{10};

    class wake_timer_registration
    {
       public:
        wake_timer_registration(signaling_client& client, boost::asio::steady_timer& timer) : client_(client), timer_(timer) {}
        ~wake_timer_registration();

       private:
        signaling_client& client_;
        boost::asio::steady_timer& timer_;
    };

   private:
    std::mutex event_mutex_;
    std::vector<runtime_event> pending_events_;
    boost::asio::steady_timer* wake_timer_{};
    std::optional<boost::asio::any_io_executor> wakeup_executor_;
    std::optional<std::chrono::steady_clock::time_point> event_deadline_;
    bool wakeup_posted_{};
    config config_;
    std::string instance_id_;
    std::chrono::milliseconds heartbeat_interval_{std::chrono::seconds{5}};
    std::chrono::milliseconds request_timeout_{std::chrono::seconds{3}};
    std::string host_;
    std::string port_;
};

}    // namespace media_server

#endif
