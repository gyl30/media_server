#ifndef MEDIA_NET_IDLE_TIMER_H
#define MEDIA_NET_IDLE_TIMER_H

#include <chrono>
#include <memory>
#include <functional>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace media_server
{

inline constexpr std::chrono::seconds media_idle_timeout{20};

class idle_timer final
{
   public:
    explicit idle_timer(boost::asio::io_context& io);

   public:
    void start(std::weak_ptr<void> owner, std::function<void()> on_idle);
    void touch() noexcept;
    void stop();

   private:
    void wait();

   private:
    boost::asio::steady_timer timer_;
    std::weak_ptr<void> owner_;
    std::chrono::steady_clock::time_point last_activity_{};
    std::function<void()> on_idle_;
};

}    // namespace media_server

#endif
