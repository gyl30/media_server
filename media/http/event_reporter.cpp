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
    reset();
    std::scoped_lock lock(mutex_);
    io_ = &io;
    server_id_ = std::move(server_id);
    instance_id_ = std::move(instance_id);
    cancellation_ = std::make_shared<boost::asio::cancellation_signal>();
}

void event_reporter::configure_mock() { reset(); }

void event_reporter::configure_handler(std::string server_id, std::string instance_id, runtime_event_handler handler)
{
    reset();
    std::scoped_lock lock(mutex_);
    server_id_ = std::move(server_id);
    instance_id_ = std::move(instance_id);
    handler_ = std::move(handler);
}

void event_reporter::report(runtime_event event)
{
    bool overflow{};
    std::size_t pending{};
    std::string newest_stream_id;
    bool start{};
    std::size_t generation{};
    boost::asio::io_context* io{};
    std::shared_ptr<boost::asio::cancellation_signal> cancellation;
    runtime_event_handler handler;
    {
        std::scoped_lock lock(mutex_);
        if (io_ == nullptr && !handler_)
        {
            return;
        }
        event.server_id = server_id_;
        event.instance_id = instance_id_;
        if (handler_)
        {
            handler = handler_;
        }
        else
        {
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
                generation = generation_;
                io = io_;
                cancellation = cancellation_;
            }
        }
    }
    if (handler)
    {
        handler(std::move(event));
        return;
    }
    if (overflow)
    {
        spdlog::warn("runtime event queue full pending {} limit {} newest stream_id {}", pending, max_pending_events, newest_stream_id);
    }
    if (start)
    {
        boost::asio::post(*io, [this, generation, cancellation]() { start_writer(generation, cancellation); });
    }
}

void event_reporter::reset()
{
    boost::asio::io_context* io{};
    std::shared_ptr<boost::asio::cancellation_signal> cancellation;
    {
        std::scoped_lock lock(mutex_);
        ++generation_;
        io = io_;
        io_ = nullptr;
        server_id_.clear();
        instance_id_.clear();
        handler_ = {};
        pending_events_.clear();
        writer_running_ = false;
        cancellation = std::move(cancellation_);
    }
    if (io != nullptr && cancellation)
    {
        boost::asio::dispatch(*io, [cancellation = std::move(cancellation)]() { cancellation->emit(boost::asio::cancellation_type::all); });
    }
}

void event_reporter::start_writer(std::size_t generation, std::shared_ptr<boost::asio::cancellation_signal> cancellation)
{
    boost::asio::io_context* io{};
    {
        std::scoped_lock lock(mutex_);
        if (generation != generation_ || io_ == nullptr)
        {
            return;
        }
        io = io_;
    }
    boost::asio::spawn(
        *io,
        [this, generation, cancellation](boost::asio::yield_context yield)
        {
            static_cast<void>(cancellation);
            run_writer(generation, yield);
        },
        boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));
}

void event_reporter::run_writer(std::size_t generation, boost::asio::yield_context yield)
{
    using namespace std::chrono_literals;

    yield.throw_if_cancelled(false);
    boost::asio::steady_timer reconnect_timer(yield.get_executor());
    for (;;)
    {
        auto event = take_next_event(generation);
        if (!event)
        {
            break;
        }
        const auto result = signaling_client::instance().report_runtime_event(*event, yield);
        if (!active(generation))
        {
            break;
        }
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
        if (error || !active(generation))
        {
            break;
        }
    }
    finish_writer(generation);
}

std::optional<runtime_event> event_reporter::take_next_event(std::size_t generation)
{
    std::scoped_lock lock(mutex_);
    if (generation != generation_ || io_ == nullptr || pending_events_.empty())
    {
        return std::nullopt;
    }
    auto event = std::move(pending_events_.front());
    pending_events_.pop_front();
    return event;
}

bool event_reporter::active(std::size_t generation)
{
    std::scoped_lock lock(mutex_);
    return generation == generation_ && io_ != nullptr;
}

void event_reporter::finish_writer(std::size_t generation)
{
    bool restart{};
    boost::asio::io_context* io{};
    std::shared_ptr<boost::asio::cancellation_signal> cancellation;
    {
        std::scoped_lock lock(mutex_);
        if (generation != generation_)
        {
            return;
        }
        writer_running_ = false;
        if (io_ != nullptr && !pending_events_.empty())
        {
            writer_running_ = true;
            restart = true;
            io = io_;
            cancellation = cancellation_;
        }
    }
    if (restart)
    {
        boost::asio::post(*io, [this, generation, cancellation]() { start_writer(generation, cancellation); });
    }
}

}    // namespace media_server
