#ifndef MEDIA_HTTP_HTTP_ASYNC_H
#define MEDIA_HTTP_HTTP_ASYNC_H

#include <atomic>
#include <memory>
#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

namespace media_server
{

// 控制接口异步操作的结果；stopped 表示服务停止或请求被取消，不代表操作已完成。
enum class async_result
{
    completed,
    not_found,
    stopped,
};

// 在 HTTP 协程中等待只提供完成回调的操作：start 收到 done(async_result)，done 可在任意线程调用。
// 业务完成与协程取消（worker 停止）先到者生效，协程只恢复一次；取消不撤销已经开始的操作，迟到的完成被忽略。
template <typename Start>
async_result await_result(Start start, boost::asio::yield_context yield)
{
    return boost::asio::async_initiate<boost::asio::yield_context, void(async_result)>(
        [](auto handler, Start initiate)
        {
            using handler_type = decltype(handler);
            struct state
            {
                explicit state(handler_type value) : handler(std::move(value)) {}

                handler_type handler;
                std::atomic_bool finished{};
            };
            const auto shared = std::make_shared<state>(std::move(handler));
            const auto executor = boost::asio::get_associated_executor(shared->handler);
            auto finish = [shared, executor](async_result result)
            {
                if (shared->finished.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                boost::asio::post(executor,
                                  [shared, result]()
                                  {
                                      // 取消槽位在协程 executor 上触发和清理，恢复前解除登记。
                                      boost::asio::get_associated_cancellation_slot(shared->handler).clear();
                                      std::move(shared->handler)(result);
                                  });
            };
            auto slot = boost::asio::get_associated_cancellation_slot(shared->handler);
            if (slot.is_connected())
            {
                slot.assign([finish](boost::asio::cancellation_type) { finish(async_result::stopped); });
            }
            initiate(std::move(finish));
        },
        yield,
        std::move(start));
}

}    // namespace media_server

#endif
