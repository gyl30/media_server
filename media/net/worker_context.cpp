#include "media/net/worker_context.h"

namespace media_server
{

worker_context::worker_context() : work_(boost::asio::make_work_guard(io_)) {}

worker_context::~worker_context()
{
    for (auto& callback : shutdown_callbacks_)
    {
        if (callback.subscription != nullptr)
        {
            callback.subscription->worker_ = nullptr;
        }
    }
    shutdown_callbacks_.clear();
}

worker_context::shutdown_subscription::shutdown_subscription(worker_context& worker, shutdown_callback_list::iterator iterator) noexcept
    : worker_(&worker), iterator_(iterator)
{
    iterator_->subscription = this;
}

worker_context::shutdown_subscription::shutdown_subscription(shutdown_subscription&& other) noexcept
    : worker_(std::exchange(other.worker_, nullptr)), iterator_(other.iterator_)
{
    if (worker_ != nullptr)
    {
        iterator_->subscription = this;
    }
}

worker_context::shutdown_subscription& worker_context::shutdown_subscription::operator=(shutdown_subscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        worker_ = std::exchange(other.worker_, nullptr);
        iterator_ = other.iterator_;
        if (worker_ != nullptr)
        {
            iterator_->subscription = this;
        }
    }
    return *this;
}

worker_context::shutdown_subscription::~shutdown_subscription() { reset(); }

worker_context::shutdown_subscription::operator bool() const noexcept { return worker_ != nullptr; }

void worker_context::shutdown_subscription::reset()
{
    if (worker_ != nullptr)
    {
        iterator_->subscription = nullptr;
        worker_->unsubscribe_shutdown(iterator_);
        worker_ = nullptr;
    }
}

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
            for (auto iterator = shutdown_callbacks_.begin(); iterator != shutdown_callbacks_.end();)
            {
                const auto current = iterator++;
                auto callback = std::move(current->function);
                if (current->subscription != nullptr)
                {
                    current->subscription->worker_ = nullptr;
                }
                shutdown_callbacks_.erase(current);
                callback();
            }
            work_.reset();
        });
}

bool worker_context::stop_requested() const noexcept { return stop_requested_.load(std::memory_order_acquire); }

worker_context::shutdown_subscription worker_context::subscribe_shutdown(std::function<void()> callback)
{
    if (stop_requested())
    {
        return {};
    }
    return {*this, shutdown_callbacks_.emplace(shutdown_callbacks_.end(), shutdown_callback{.function = std::move(callback)})};
}

void worker_context::unsubscribe_shutdown(shutdown_callback_list::iterator iterator) { shutdown_callbacks_.erase(iterator); }

void worker_context::stop() { io_.stop(); }

void worker_context::release_work() { work_.reset(); }

void worker_context::run() { io_.run(); }

}    // namespace media_server
