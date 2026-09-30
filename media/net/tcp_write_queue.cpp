#include <utility>

#include "media/net/tcp_write_queue.h"

namespace media_server
{

tcp_write_queue::tcp_write_queue(std::size_t max_bytes) : max_bytes_(max_bytes) {}

tcp_write_enqueue_result tcp_write_queue::enqueue(buffer data)
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
    entries_.push_back(std::move(data));
    return start_writer ? tcp_write_enqueue_result::start_writer : tcp_write_enqueue_result::queued;
}

boost::system::error_code tcp_write_queue::write_one(tcp_yield_transport& transport, boost::asio::yield_context& yield)
{
    const auto queued_data = entries_.front();
    boost::system::error_code error;
    transport.write(*queued_data, yield, error);
    if (error)
    {
        stopped_ = true;
    }
    else
    {
        queued_bytes_ -= queued_data->size();
        entries_.pop_front();
    }
    return error;
}

bool tcp_write_queue::empty() const noexcept { return entries_.empty(); }

bool tcp_write_queue::stopped() const noexcept { return stopped_; }

std::size_t tcp_write_queue::queued_bytes() const noexcept { return queued_bytes_; }

std::size_t tcp_write_queue::max_bytes() const noexcept { return max_bytes_; }

void tcp_write_queue::stop() noexcept { stopped_ = true; }

}    // namespace media_server
