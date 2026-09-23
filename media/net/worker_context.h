#ifndef MEDIA_NET_WORKER_CONTEXT_H
#define MEDIA_NET_WORKER_CONTEXT_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <list>
#include <utility>

namespace media_server
{

class worker_context final
{
   public:
    class shutdown_subscription;

   private:
    struct shutdown_callback
    {
        std::function<void()> function;
        shutdown_subscription* subscription{};
    };

    using shutdown_callback_list = std::list<shutdown_callback>;

   public:
    class shutdown_subscription final
    {
       public:
        shutdown_subscription() = default;
        shutdown_subscription(const shutdown_subscription&) = delete;
        shutdown_subscription& operator=(const shutdown_subscription&) = delete;
        shutdown_subscription(shutdown_subscription&& other) noexcept;
        shutdown_subscription& operator=(shutdown_subscription&& other) noexcept;
        ~shutdown_subscription();

        explicit operator bool() const noexcept;
        void reset();

       private:
        friend class worker_context;

        shutdown_subscription(worker_context& worker, shutdown_callback_list::iterator iterator) noexcept;

        worker_context* worker_{};
        shutdown_callback_list::iterator iterator_{};
    };

    worker_context();
    ~worker_context();

   public:
    [[nodiscard]] boost::asio::io_context& io() noexcept;

    template <typename Function>
    void spawn(Function function)
    {
        if (stop_requested_.load(std::memory_order_acquire))
        {
            return;
        }

        boost::asio::dispatch(io_, [this, function = std::move(function)]() mutable { spawn_on_owner(std::move(function)); });
    }

    void request_stop();
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] std::size_t active_task_count() const noexcept;
    [[nodiscard]] shutdown_subscription subscribe_shutdown(std::function<void()> callback);

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

    void unsubscribe_shutdown(shutdown_callback_list::iterator iterator);

    boost::asio::io_context io_{1};
    work_guard work_;
    std::list<task> tasks_;
    shutdown_callback_list shutdown_callbacks_;
    std::atomic_bool stop_requested_{};
    std::atomic_size_t active_task_count_{};
};

}    // namespace media_server

#endif
