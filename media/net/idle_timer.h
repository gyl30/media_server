#ifndef MEDIA_NET_IDLE_TIMER_H
#define MEDIA_NET_IDLE_TIMER_H

#include <chrono>
#include <memory>
#include <functional>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace media_server
{

// 输入空闲期限：从建立开始计时，网络读到数据即刷新；超过该时长没有输入则关闭会话。
inline constexpr std::chrono::seconds media_idle_timeout{20};

// 由 owner executor 使用；owner 必须持有本对象，回调前通过 weak owner 确认其仍然存活，回调可直接使用 owner。
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
