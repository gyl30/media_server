#include <chrono>
#include <utility>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include "media/http/event_reporter.h"
#include "media/http/signaling_client.h"

namespace media_server
{

event_reporter& event_reporter::instance()
{
    static event_reporter value;
    return value;
}

void event_reporter::configure(boost::asio::io_context& io, std::string server_id, std::string instance_id)
{
    std::scoped_lock lock(mutex_);
    io_ = &io;
    server_id_ = std::move(server_id);
    instance_id_ = std::move(instance_id);
}

void event_reporter::report(runtime_event event)
{
    bool overflow{};
    std::size_t pending{};
    std::string newest_stream_id;
    bool start{};
    boost::asio::io_context* io{};
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return;
        }
        event.server_id = server_id_;
        event.instance_id = instance_id_;
        if (pending_events_.size() >= max_pending_events)
        {
            overflow = true;
            pending = pending_events_.size();
            newest_stream_id = event.stream_id;
            pending_events_.clear();
        }
        pending_events_.push_back(std::move(event));
        if (!writer_running_)
        {
            writer_running_ = true;
            start = true;
            io = io_;
        }
    }
    if (overflow)
    {
        spdlog::warn("runtime event queue full pending {} limit {} newest stream_id {}", pending, max_pending_events, newest_stream_id);
    }
    if (start)
    {
        boost::asio::post(*io, [this]() { start_writer(); });
    }
}

void event_reporter::start_writer()
{
    boost::asio::io_context* io{};
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr)
        {
            return;
        }
        io = io_;
    }
    boost::asio::spawn(*io, [this](boost::asio::yield_context yield) { run_writer(yield); }, boost::asio::detached);
}

void event_reporter::run_writer(boost::asio::yield_context yield)
{
    using namespace std::chrono_literals;

    yield.throw_if_cancelled(false);
    boost::asio::steady_timer reconnect_timer(yield.get_executor());
    for (;;)
    {
        auto event = take_next_event();
        if (!event)
        {
            break;
        }
        const auto result = signaling_client::instance().report_runtime_event(*event, yield);
        if (result.kind == signaling_result_kind::accepted)
        {
            continue;
        }
        if (result.kind != signaling_result_kind::network_error)
        {
            spdlog::warn("runtime event delivery failed stream {} status {}", event->stream_id, result.status);
            continue;
        }

        spdlog::warn("runtime event delivery network error stream {} error {}; reconnecting in 5 seconds", event->stream_id, result.error);
        reconnect_timer.expires_after(5s);
        boost::system::error_code error;
        reconnect_timer.async_wait(yield[error]);
        if (error)
        {
            break;
        }
    }
    finish_writer();
}

std::optional<runtime_event> event_reporter::take_next_event()
{
    std::scoped_lock lock(mutex_);
    if (pending_events_.empty())
    {
        return std::nullopt;
    }
    auto event = std::move(pending_events_.front());
    pending_events_.pop_front();
    return event;
}

void event_reporter::finish_writer()
{
    bool restart{};
    boost::asio::io_context* io{};
    {
        std::scoped_lock lock(mutex_);
        writer_running_ = false;
        if (!pending_events_.empty())
        {
            writer_running_ = true;
            restart = true;
            io = io_;
        }
    }
    if (restart)
    {
        boost::asio::post(*io, [this]() { start_writer(); });
    }
}

}    // namespace media_server
