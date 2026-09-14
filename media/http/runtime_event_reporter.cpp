#include <chrono>
#include <utility>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include "media/http/runtime_event_reporter.h"
#include "media/http/signaling_client.h"

namespace media_server
{

runtime_event_reporter::runtime_event_reporter(boost::asio::io_context& io, std::shared_ptr<const signaling_client> signaling)
    : io_(io), signaling_(std::move(signaling))
{
}

void runtime_event_reporter::report(runtime_event event)
{
    boost::asio::post(io_, [self = shared_from_this(), event = std::move(event)]() mutable { self->enqueue(std::move(event)); });
}

void runtime_event_reporter::shutdown()
{
    boost::asio::dispatch(io_, [self = shared_from_this()]() { self->safe_shutdown(); });
}

void runtime_event_reporter::enqueue(runtime_event event)
{
    if (closed_)
    {
        return;
    }
    if (pending_events_.size() >= max_pending_events)
    {
        spdlog::warn("runtime event queue full pending {} limit {} newest stream_id {}",
                     pending_events_.size(),
                     max_pending_events,
                     event.stream_id);
        pending_events_.clear();
    }
    pending_events_.push_back(std::move(event));
    if (!writer_running_)
    {
        start_writer();
    }
}

void runtime_event_reporter::start_writer()
{
    writer_running_ = true;
    boost::asio::spawn(
        io_,
        [self = shared_from_this()](boost::asio::yield_context yield) { self->run_writer(yield); },
        boost::asio::bind_cancellation_slot(cancellation_.slot(), boost::asio::detached));
}

void runtime_event_reporter::run_writer(boost::asio::yield_context yield)
{
    using namespace std::chrono_literals;

    yield.throw_if_cancelled(false);
    boost::asio::steady_timer reconnect_timer(yield.get_executor());
    while (!closed_ && !pending_events_.empty())
    {
        auto event = std::move(pending_events_.front());
        pending_events_.pop_front();
        const auto result = signaling_->report_runtime_event(event, yield);
        if (closed_)
        {
            break;
        }
        if (result.kind == signaling_result_kind::accepted)
        {
            continue;
        }
        if (result.kind != signaling_result_kind::network_error)
        {
            spdlog::warn("runtime event delivery failed stream {} status {}", event.stream_id, result.status);
            continue;
        }

        spdlog::warn("runtime event delivery network error stream {} error {}; reconnecting in 5 seconds",
                     event.stream_id,
                     result.error);
        reconnect_timer.expires_after(5s);
        boost::system::error_code error;
        reconnect_timer.async_wait(yield[error]);
        if (error || closed_)
        {
            break;
        }
    }
    writer_running_ = false;
}

void runtime_event_reporter::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    cancellation_.emit(boost::asio::cancellation_type::all);
}

}    // namespace media_server
