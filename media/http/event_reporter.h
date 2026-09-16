#ifndef MEDIA_HTTP_EVENT_REPORTER_H
#define MEDIA_HTTP_EVENT_REPORTER_H

#include <deque>
#include <mutex>
#include <string>
#include <cstddef>
#include <optional>

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/runtime_event.h"

namespace media_server
{

class event_reporter final
{
   public:
    [[nodiscard]] static event_reporter& instance();

    void configure(boost::asio::io_context& io, std::string server_id, std::string instance_id);

    void report(runtime_event event);

   private:
    void start_writer();
    void run_writer(boost::asio::yield_context yield);
    [[nodiscard]] std::optional<runtime_event> take_next_event();
    void finish_writer();

   private:
    static constexpr std::size_t max_pending_events = 500U;
    std::mutex mutex_;
    std::deque<runtime_event> pending_events_;
    boost::asio::io_context* io_{};
    std::string server_id_;
    std::string instance_id_;
    bool writer_running_{};
};

}    // namespace media_server

#endif
