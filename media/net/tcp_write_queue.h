#ifndef MEDIA_NET_TCP_WRITE_QUEUE_H
#define MEDIA_NET_TCP_WRITE_QUEUE_H

#include <deque>
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdint>

#include "media/net/tcp_yield_transport.h"

namespace media_server
{

enum class tcp_write_enqueue_result
{
    overflow,
    stopped,
    queued,
    start_writer,
};

struct tcp_write_result
{
    boost::system::error_code error;
    bool stop_after_write{};
};

class tcp_write_queue final
{
   public:
    using buffer = std::shared_ptr<std::vector<std::uint8_t>>;

    explicit tcp_write_queue(std::size_t max_bytes);

    [[nodiscard]] tcp_write_enqueue_result enqueue(buffer data, bool stop_after_write = false);
    [[nodiscard]] tcp_write_result write_one(tcp_yield_transport& transport, boost::asio::yield_context& yield);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    void stop() noexcept;

   private:
    struct entry
    {
        buffer data;
        bool stop_after_write{};
    };

    std::size_t max_bytes_;
    std::size_t queued_bytes_{};
    std::deque<entry> entries_;
    bool stopped_{};
};

}    // namespace media_server

#endif
