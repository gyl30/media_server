#ifndef MEDIA_NET_WORKER_POOL_H
#define MEDIA_NET_WORKER_POOL_H

#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <cstddef>

#include <boost/asio/io_context.hpp>

#include "media/net/worker_context.h"

namespace media_server
{
class worker_pool;

class worker_lease final
{
   public:
    worker_lease(worker_lease&& other) noexcept;
    worker_lease(const worker_lease&) = delete;
    worker_lease& operator=(const worker_lease&) = delete;
    worker_lease& operator=(worker_lease&&) = delete;
    ~worker_lease();

    [[nodiscard]] worker_context& worker() const noexcept;

   private:
    friend class worker_pool;

    worker_lease(worker_pool& pool, std::size_t index) noexcept;

    worker_pool* pool_;
    std::size_t index_;
};

class worker_pool final
{
   public:
    explicit worker_pool(std::size_t size);

   public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] worker_context& context(std::size_t index) noexcept;
    [[nodiscard]] worker_context& next() noexcept;
    [[nodiscard]] worker_lease lease();

    void request_stop();
    void run();

   private:
    friend class worker_lease;

    void release(std::size_t index);

    std::mutex lease_mutex_;
    std::vector<std::size_t> leases_;
    std::vector<std::unique_ptr<worker_context>> contexts_;
    std::atomic<std::size_t> next_{};
};

}    // namespace media_server

#endif
