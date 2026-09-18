#include <utility>

#include "media/net/tcp_write_queue.h"

namespace media_server
{

tcp_write_queue::tcp_write_queue(std::size_t max_bytes) : max_bytes_(max_bytes) {}

tcp_write_enqueue_result tcp_write_queue::enqueue(buffer data, bool stop_after_write)
{
    if (stopped_)
    {
        return tcp_write_enqueue_result::stopped;
    }
    if (data->size() > max_bytes_ || queued_bytes_ > max_bytes_ - data->size())
    {
        stopped_ = true;
        return tcp_write_enqueue_result::overflow;
    }

    const bool start_writer = entries_.empty();
    queued_bytes_ += data->size();
    entries_.push_back({.data = std::move(data), .stop_after_write = stop_after_write});
    return start_writer ? tcp_write_enqueue_result::start_writer : tcp_write_enqueue_result::queued;
}

tcp_write_result tcp_write_queue::write_one(tcp_yield_transport& transport, boost::asio::yield_context& yield)
{
    const auto queued_entry = entries_.front();
    boost::system::error_code error;
    transport.write(*queued_entry.data, yield, error);
    if (error)
    {
        stopped_ = true;
    }
    else
    {
        queued_bytes_ -= queued_entry.data->size();
        entries_.pop_front();
    }
    return {.error = error, .stop_after_write = queued_entry.stop_after_write};
}

bool tcp_write_queue::empty() const noexcept { return entries_.empty(); }

bool tcp_write_queue::stopped() const noexcept { return stopped_; }

void tcp_write_queue::stop() noexcept { stopped_ = true; }

}    // namespace media_server
