#ifndef MEDIA_HTTP_HTTP_ASYNC_H
#define MEDIA_HTTP_HTTP_ASYNC_H

#include <memory>
#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

namespace media_server
{

// 在 HTTP 协程中等待只提供完成回调的操作：start 收到 done(bool)，done 可在任意线程调用一次，
// 协程在自己的 executor 上恢复并返回该结果。用于让控制接口的响应代表操作实际完成。
template <typename Start>
bool await_result(Start start, boost::asio::yield_context yield)
{
    return boost::asio::async_initiate<boost::asio::yield_context, void(bool)>(
        [](auto handler, Start initiate)
        {
            auto shared = std::make_shared<decltype(handler)>(std::move(handler));
            const auto executor = boost::asio::get_associated_executor(*shared);
            initiate([shared, executor](bool result) { boost::asio::post(executor, [shared, result]() { std::move(*shared)(result); }); });
        },
        yield,
        std::move(start));
}

}    // namespace media_server

#endif
