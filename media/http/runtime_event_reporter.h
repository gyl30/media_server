#ifndef MEDIA_HTTP_RUNTIME_EVENT_REPORTER_H
#define MEDIA_HTTP_RUNTIME_EVENT_REPORTER_H

#include <cstddef>
#include <deque>
#include <memory>

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>

#include "media/core/runtime_event.h"

namespace media_server
{

class signaling_client;

class runtime_event_reporter final : public std::enable_shared_from_this<runtime_event_reporter>
{
   public:
    runtime_event_reporter(boost::asio::io_context& io, std::shared_ptr<const signaling_client> signaling);

    runtime_event_reporter(const runtime_event_reporter&) = delete;
    runtime_event_reporter& operator=(const runtime_event_reporter&) = delete;

    void report(runtime_event event);
    void shutdown();

   private:
    static constexpr std::size_t max_pending_events = 500U;

    void enqueue(runtime_event event);
    void start_writer();
    void run_writer(boost::asio::yield_context yield);
    void safe_shutdown();

    boost::asio::io_context& io_;
    std::shared_ptr<const signaling_client> signaling_;
    std::deque<runtime_event> pending_events_;
    boost::asio::cancellation_signal cancellation_;
    bool writer_running_{};
    bool closed_{};
};

}    // namespace media_server

#endif
