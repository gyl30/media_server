#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "media/http/http_async.h"
#include "media/net/worker_context.h"

namespace
{
using namespace media_server;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// 业务完成先到：协程得到业务结果。
void completes_once()
{
    worker_context worker;
    std::thread thread([&worker]() { worker.run(); });
    std::promise<async_result> result;
    std::thread producer;
    worker.spawn(
        [&](boost::asio::yield_context yield)
        {
            result.set_value(await_result(
                [&producer](auto done) { producer = std::thread([done]() { done(async_result::completed); done(async_result::not_found); }); },
                yield));
        });
    const auto value = result.get_future().get();
    producer.join();
    worker.request_stop();
    thread.join();
    require(value == async_result::completed, "business completion not delivered");
}

// 等待期间 worker 停止：协程以 stopped 恢复一次，之后迟到的业务完成被忽略。
void cancel_then_late_completion()
{
    worker_context worker;
    std::thread thread([&worker]() { worker.run(); });
    std::promise<std::function<void(async_result)>> started;
    std::atomic_int resumed{};
    std::promise<async_result> result;
    worker.spawn(
        [&](boost::asio::yield_context yield)
        {
            const auto value = await_result([&started](auto done) { started.set_value(done); }, yield);
            ++resumed;
            result.set_value(value);
        });
    const auto done = started.get_future().get();
    worker.request_stop();
    auto future = result.get_future();
    const bool ended = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    // 迟到的业务完成；取消未生效时它也负责让协程结束，保证测试线程可以回收。
    done(async_result::completed);
    thread.join();
    require(ended, "cancellation did not end wait");
    require(future.get() == async_result::stopped, "cancellation result is not stopped");
    require(resumed == 1, "coroutine resumed more than once");
}
}    // namespace

int main()
{
    try
    {
        completes_once();
        cancel_then_late_completion();
        std::cout << "http async: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
