#include "media/net/worker_context.h"

namespace media_server
{

worker_context::worker_context() : work_(boost::asio::make_work_guard(io_)) {}

boost::asio::io_context& worker_context::io() noexcept { return io_; }

void worker_context::request_stop()
{
    if (stop_requested_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    boost::asio::post(
        io_,
        [this]()
        {
            for (auto iterator = tasks_.begin(); iterator != tasks_.end();)
            {
                const auto current = iterator++;
                current->cancellation.emit(boost::asio::cancellation_type::terminal);
            }
            work_.reset();
        });
}

bool worker_context::stop_requested() const noexcept { return stop_requested_.load(std::memory_order_acquire); }

std::size_t worker_context::active_task_count() const noexcept { return active_task_count_.load(std::memory_order_acquire); }

void worker_context::stop() { io_.stop(); }

void worker_context::release_work() { work_.reset(); }

void worker_context::run() { io_.run(); }

}    // namespace media_server
