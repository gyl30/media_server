#include <thread>
#include <utility>
#include <algorithm>

#include "media/net/worker_pool.h"

namespace media_server
{

worker_lease::worker_lease(worker_pool& pool, std::size_t index) noexcept : pool_(&pool), index_(index) {}

worker_lease::worker_lease(worker_lease&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}

worker_lease::~worker_lease()
{
    if (pool_ != nullptr)
    {
        pool_->release(index_);
    }
}

worker_context& worker_lease::worker() const noexcept { return pool_->context(index_); }

worker_pool::worker_pool(std::size_t size) : leases_(size)
{
    contexts_.reserve(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        contexts_.push_back(std::make_unique<worker_context>());
    }
}

std::size_t worker_pool::size() const noexcept { return contexts_.size(); }

worker_context& worker_pool::context(std::size_t index) noexcept { return *contexts_[index]; }

worker_context& worker_pool::next() noexcept
{
    const auto index = next_.fetch_add(1U, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[index];
}

worker_lease worker_pool::lease()
{
    std::scoped_lock lock(lease_mutex_);
    const auto index = static_cast<std::size_t>(std::ranges::min_element(leases_) - leases_.begin());
    ++leases_[index];
    return worker_lease(*this, index);
}

void worker_pool::release(std::size_t index)
{
    std::scoped_lock lock(lease_mutex_);
    --leases_[index];
}

void worker_pool::request_stop()
{
    for (const auto& context : contexts_)
    {
        context->request_stop();
    }
}

void worker_pool::run()
{
    std::vector<std::thread> threads;
    threads.reserve(contexts_.size() - 1U);
    for (std::size_t index = 1; index < contexts_.size(); ++index)
    {
        threads.emplace_back([this, index]() { contexts_[index]->run(); });
    }

    contexts_.front()->run();
    for (auto& thread : threads)
    {
        thread.join();
    }
}

}    // namespace media_server
