#include <thread>

#include "media/net/io_context_pool.h"

namespace media_server
{

io_context_pool::io_context_pool(std::size_t size)
{
    contexts_.reserve(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        contexts_.push_back(std::make_unique<worker_context>());
    }
}

std::size_t io_context_pool::size() const noexcept { return contexts_.size(); }

boost::asio::io_context& io_context_pool::context(std::size_t index) noexcept { return contexts_[index]->io(); }

boost::asio::io_context& io_context_pool::next() noexcept
{
    const auto index = next_.fetch_add(1U, std::memory_order_relaxed) % contexts_.size();
    return contexts_[index]->io();
}

void io_context_pool::stop()
{
    for (const auto& context : contexts_)
    {
        context->stop();
    }
}

void io_context_pool::release_work()
{
    for (const auto& context : contexts_)
    {
        context->release_work();
    }
}

void io_context_pool::run()
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
