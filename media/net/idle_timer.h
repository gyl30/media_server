#ifndef MEDIA_NET_IDLE_TIMER_H
#define MEDIA_NET_IDLE_TIMER_H

#include <chrono>
#include <memory>
#include <functional>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace media_server
{

// 输入会话超过该时间没有收到媒体即视为上游失效。
inline constexpr std::chrono::seconds media_idle_timeout{20};

// 由 owner executor 使用；owner 必须持有本对象，回调前通过 weak owner 确认其仍然存活。
class idle_timer final
{
   public:
    explicit idle_timer(boost::asio::io_context& io);

   public:
    void start(std::weak_ptr<void> owner, std::chrono::steady_clock::duration timeout, std::function<void()> on_idle);
    void touch() noexcept;
    void stop();

   private:
    void wait();

   private:
    boost::asio::steady_timer timer_;
    std::weak_ptr<void> owner_;
    std::chrono::steady_clock::duration timeout_{};
    std::chrono::steady_clock::time_point last_activity_{};
    std::function<void()> on_idle_;
};

}    // namespace media_server

#endif
