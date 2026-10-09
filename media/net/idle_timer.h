#ifndef MEDIA_NET_IDLE_TIMER_H
#define MEDIA_NET_IDLE_TIMER_H

#include <chrono>
#include <memory>
#include <functional>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace media_server
{

// 输入空闲期限：连接或会话建立后该时间内必须发布出首个媒体帧，此后任意该时长内都要持续发布。
// 以"发布出媒体帧"为准，才能统一排除 RTCP、空载荷、被忽略的消息等非媒体输入；
// 解复用器缓冲一个访问单元造成的出帧滞后计入期限。
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
