#ifndef MEDIA_NET_IO_CONTEXT_POOL_H
#define MEDIA_NET_IO_CONTEXT_POOL_H

#include <atomic>
#include <memory>
#include <vector>
#include <cstddef>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

namespace media_server
{

class io_context_pool final
{
   public:
    explicit io_context_pool(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] boost::asio::io_context& context(std::size_t index) noexcept;
    [[nodiscard]] boost::asio::io_context& next() noexcept;

    void stop();
    void release_work();
    void run();

   private:
    using work_guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    std::vector<std::unique_ptr<boost::asio::io_context>> contexts_;
    std::vector<work_guard> work_;
    std::atomic<std::size_t> next_{};
};

}    // namespace media_server

#endif
