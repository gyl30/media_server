#ifndef MEDIA_HTTP_EVENT_REPORTER_H
#define MEDIA_HTTP_EVENT_REPORTER_H

#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <cstddef>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/cancellation_signal.hpp>

#include "media/core/runtime_event.h"

namespace media_server
{

class event_reporter final
{
   public:
    [[nodiscard]] static event_reporter& instance();

    void configure(boost::asio::io_context& io, std::string server_id, std::string instance_id);
    void configure_mock();
    void configure_handler(std::string server_id, std::string instance_id, runtime_event_handler handler);

    void report(runtime_event event);

   private:
    void reset();
    void start_writer(std::size_t generation, std::shared_ptr<boost::asio::cancellation_signal> cancellation);
    void run_writer(std::size_t generation, boost::asio::yield_context yield);
    [[nodiscard]] std::optional<runtime_event> take_next_event(std::size_t generation);
    [[nodiscard]] bool active(std::size_t generation);
    void finish_writer(std::size_t generation);

   private:
    static constexpr std::size_t max_pending_events = 500U;
    std::mutex mutex_;
    std::deque<runtime_event> pending_events_;
    std::shared_ptr<boost::asio::cancellation_signal> cancellation_;
    runtime_event_handler handler_;
    boost::asio::io_context* io_{};
    std::string server_id_;
    std::string instance_id_;
    std::size_t generation_{};
    bool writer_running_{};
};

}    // namespace media_server

#endif
