#ifndef MEDIA_NET_WORKER_CONTEXT_H
#define MEDIA_NET_WORKER_CONTEXT_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <exception>
#include <list>
#include <utility>

namespace media_server
{

class worker_context final
{
   public:
    worker_context();

   public:
    [[nodiscard]] boost::asio::io_context& io() noexcept;

    template <typename Function>
    void spawn(Function function)
    {
        if (stop_requested_.load(std::memory_order_acquire))
        {
            return;
        }

        boost::asio::post(io_, [this, function = std::move(function)]() mutable { spawn_on_owner(std::move(function)); });
    }

    void request_stop();
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] std::size_t active_task_count() const noexcept;

    void stop();
    void release_work();
    void run();

   private:
    using work_guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    struct task
    {
        boost::asio::cancellation_signal cancellation;
    };

    template <typename Function>
    void spawn_on_owner(Function function)
    {
        if (stop_requested_.load(std::memory_order_acquire))
        {
            return;
        }

        const auto task_iterator = tasks_.emplace(tasks_.end());
        active_task_count_.fetch_add(1U, std::memory_order_release);
        boost::asio::spawn(
            io_,
            std::move(function),
            boost::asio::bind_cancellation_slot(
                task_iterator->cancellation.slot(),
                boost::asio::bind_executor(
                    io_.get_executor(),
                    [this, task_iterator](std::exception_ptr)
                    {
                        tasks_.erase(task_iterator);
                        active_task_count_.fetch_sub(1U, std::memory_order_release);
                    })));
    }

    boost::asio::io_context io_{1};
    work_guard work_;
    std::list<task> tasks_;
    std::atomic_bool stop_requested_{};
    std::atomic_size_t active_task_count_{};
};

}    // namespace media_server

#endif
