#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/cancel_after.hpp>

#include "media/net/io_context_pool.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void test_request_stop_drains_spawned_operations()
{
    using tcp = boost::asio::ip::tcp;

    worker_context worker;
    tcp::acceptor pending_acceptor(worker.io(), {boost::asio::ip::address_v4::loopback(), 0});
    std::promise<void> accept_started_signal;
    auto accept_started = accept_started_signal.get_future();
    std::promise<void> read_started_signal;
    auto read_started = read_started_signal.get_future();
    std::atomic_uint aborted{};
    std::atomic_bool spawned_after_stop{};
    std::vector<std::thread::id> completion_threads;

    worker.spawn(
        [&pending_acceptor, &accept_started_signal, &aborted, &completion_threads](boost::asio::yield_context yield)
        {
            tcp::socket socket(yield.get_executor());
            accept_started_signal.set_value();
            boost::system::error_code error;
            pending_acceptor.async_accept(socket, yield[error]);
            if (error == boost::asio::error::operation_aborted)
            {
                ++aborted;
            }
            completion_threads.push_back(std::this_thread::get_id());
        });

    worker.spawn(
        [&aborted, &completion_threads](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer timer(yield.get_executor(), std::chrono::hours{1});
            boost::system::error_code error;
            timer.async_wait(yield[error]);
            if (error == boost::asio::error::operation_aborted)
            {
                ++aborted;
            }
            completion_threads.push_back(std::this_thread::get_id());
        });

    boost::asio::io_context peer_io;
    tcp::acceptor peer_acceptor(peer_io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto peer_endpoint = peer_acceptor.local_endpoint();
    auto peer = std::make_shared<tcp::socket>(peer_io);
    std::promise<void> peer_connected_signal;
    auto peer_connected = peer_connected_signal.get_future();
    peer_acceptor.async_accept(*peer, [&peer_connected_signal](const boost::system::error_code& error)
                               {
                                   if (error)
                                   {
                                       throw boost::system::system_error(error);
                                   }
                                   peer_connected_signal.set_value();
                               });
    std::jthread peer_runner([&peer_io]() { peer_io.run(); });

    worker.spawn(
        [peer_endpoint, &read_started_signal, &aborted, &completion_threads](boost::asio::yield_context yield)
        {
            tcp::socket socket(yield.get_executor());
            boost::system::error_code error;
            socket.async_connect(peer_endpoint, yield[error]);
            if (error)
            {
                throw boost::system::system_error(error);
            }

            read_started_signal.set_value();
            std::array<std::uint8_t, 1> buffer{};
            socket.async_read_some(boost::asio::buffer(buffer), yield[error]);
            if (error == boost::asio::error::operation_aborted)
            {
                ++aborted;
            }
            completion_threads.push_back(std::this_thread::get_id());
        });

    std::promise<void> returned_signal;
    auto returned = returned_signal.get_future();
    std::thread::id worker_thread;
    std::jthread runner(
        [&worker, &returned_signal, &worker_thread]
        {
            worker_thread = std::this_thread::get_id();
            worker.run();
            returned_signal.set_value();
        });

    accept_started.get();
    peer_connected.get();
    read_started.get();

    worker.request_stop();
    worker.spawn([&spawned_after_stop](boost::asio::yield_context) { spawned_after_stop.store(true, std::memory_order_release); });
    const bool returned_in_time = returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!returned_in_time)
    {
        worker.stop();
    }
    runner.join();

    require(returned_in_time, "request_stop lets io_context::run return naturally");
    require(worker.stop_requested(), "request_stop records stop state");
    require(aborted.load(std::memory_order_acquire) == 3U, "request_stop cancels accept read and timer");
    require(completion_threads.size() == 3U, "all canceled coroutines resumed");
    for (const auto completion_thread : completion_threads)
    {
        require(completion_thread == worker_thread, "coroutine completes on worker thread");
    }
    require(!spawned_after_stop.load(std::memory_order_acquire), "request_stop rejects new coroutine");
}

void test_request_stop_runs_callback_shutdown_on_worker()
{
    worker_context worker;
    std::promise<void> started_signal;
    auto started = started_signal.get_future();
    std::promise<void> returned_signal;
    auto returned = returned_signal.get_future();
    std::optional<worker_context::shutdown_subscription> subscription;
    std::optional<boost::asio::steady_timer> timer;
    std::atomic_bool cancelled{};
    std::thread::id callback_thread;
    std::thread::id worker_thread;

    boost::asio::post(
        worker.io(),
        [&]()
        {
            timer.emplace(worker.io(), std::chrono::hours{1});
            subscription.emplace(worker.subscribe_shutdown(
                [&]()
                {
                    callback_thread = std::this_thread::get_id();
                    timer->cancel();
                    subscription->reset();
                }));
            require(static_cast<bool>(*subscription), "callback shutdown registered before stop");
            timer->async_wait(
                [&](const boost::system::error_code& error)
                {
                    cancelled.store(error == boost::asio::error::operation_aborted, std::memory_order_release);
                    subscription->reset();
                });
            started_signal.set_value();
        });

    std::jthread runner(
        [&]()
        {
            worker_thread = std::this_thread::get_id();
            worker.run();
            returned_signal.set_value();
        });
    started.get();
    worker.request_stop();
    const bool returned_in_time = returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!returned_in_time)
    {
        worker.stop();
    }
    runner.join();

    require(returned_in_time, "request_stop drains callback-owned operation");
    require(cancelled.load(std::memory_order_acquire), "callback shutdown cancels pending timer");
    require(callback_thread == worker_thread, "callback shutdown runs on worker thread");
}

void test_pool_request_stop_drains_each_worker()
{
    io_context_pool workers(2);
    std::promise<void> first_started_signal;
    auto first_started = first_started_signal.get_future();
    std::promise<void> second_started_signal;
    auto second_started = second_started_signal.get_future();
    std::atomic_uint aborted{};

    workers.context(0).spawn(
        [&first_started_signal, &aborted](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer timer(yield.get_executor(), std::chrono::hours{1});
            first_started_signal.set_value();
            boost::system::error_code error;
            timer.async_wait(yield[error]);
            if (error == boost::asio::error::operation_aborted)
            {
                ++aborted;
            }
        });
    workers.context(1).spawn(
        [&second_started_signal, &aborted](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer timer(yield.get_executor(), std::chrono::hours{1});
            second_started_signal.set_value();
            boost::system::error_code error;
            timer.async_wait(yield[error]);
            if (error == boost::asio::error::operation_aborted)
            {
                ++aborted;
            }
        });

    std::promise<void> returned_signal;
    auto returned = returned_signal.get_future();
    std::jthread runner(
        [&]()
        {
            workers.run();
            returned_signal.set_value();
        });
    first_started.get();
    second_started.get();
    workers.request_stop();
    const bool returned_in_time = returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!returned_in_time)
    {
        workers.stop();
    }
    runner.join();

    require(returned_in_time, "pool request_stop lets every worker return naturally");
    require(aborted.load(std::memory_order_acquire) == 2U, "pool request_stop cancels work on every worker");
}

void test_cancellation_state_distinguishes_worker_stop_from_timeout()
{
    worker_context worker;
    std::promise<void> worker_wait_started_signal;
    auto worker_wait_started = worker_wait_started_signal.get_future();
    std::promise<void> worker_wait_finished_signal;
    auto worker_wait_finished = worker_wait_finished_signal.get_future();
    std::atomic_bool worker_cancelled{};

    worker.spawn(
        [&worker_wait_started_signal, &worker_wait_finished_signal, &worker_cancelled](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer timer(yield.get_executor(), std::chrono::hours{1});
            worker_wait_started_signal.set_value();
            boost::system::error_code error;
            timer.async_wait(yield[error]);
            worker_cancelled.store(yield.cancelled() != boost::asio::cancellation_type::none, std::memory_order_release);
            require(error == boost::asio::error::operation_aborted, "worker cancellation aborts pending operation");
            worker_wait_finished_signal.set_value();
        });

    std::promise<void> worker_returned_signal;
    auto worker_returned = worker_returned_signal.get_future();
    std::jthread worker_runner([&]() {
        worker.run();
        worker_returned_signal.set_value();
    });
    worker_wait_started.get();
    worker.request_stop();
    worker_wait_finished.get();
    require(worker_returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready, "worker cancellation drains timer coroutine");
    worker_runner.join();
    require(worker_cancelled.load(std::memory_order_acquire), "worker cancellation marks yield cancellation state");

    worker_context timed_worker;
    std::promise<void> timed_wait_started_signal;
    auto timed_wait_started = timed_wait_started_signal.get_future();
    std::promise<void> timed_wait_finished_signal;
    auto timed_wait_finished = timed_wait_finished_signal.get_future();
    std::atomic_bool timed_worker_cancelled{};
    std::atomic_bool timed_worker_aborted{};
    timed_worker.spawn(
        [&timed_wait_started_signal, &timed_wait_finished_signal, &timed_worker_cancelled, &timed_worker_aborted](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer operation_timer(yield.get_executor(), std::chrono::hours{1});
            boost::asio::steady_timer cancellation_timer(yield.get_executor());
            timed_wait_started_signal.set_value();
            boost::system::error_code error;
            operation_timer.async_wait(boost::asio::cancel_after(cancellation_timer, std::chrono::hours{1}, yield[error]));
            timed_worker_aborted.store(error == boost::asio::error::operation_aborted, std::memory_order_release);
            timed_worker_cancelled.store(yield.cancelled() != boost::asio::cancellation_type::none, std::memory_order_release);
            timed_wait_finished_signal.set_value();
        });
    std::promise<void> timed_worker_returned_signal;
    auto timed_worker_returned = timed_worker_returned_signal.get_future();
    std::jthread timed_worker_runner([&]() {
        timed_worker.run();
        timed_worker_returned_signal.set_value();
    });
    timed_wait_started.get();
    timed_worker.request_stop();
    timed_wait_finished.get();
    require(timed_worker_returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready,
            "worker cancellation drains cancel_after operation");
    timed_worker_runner.join();
    require(timed_worker_aborted.load(std::memory_order_acquire), "worker cancellation aborts cancel_after operation");
    require(timed_worker_cancelled.load(std::memory_order_acquire), "worker cancellation survives cancel_after adapter");

    worker_context timeout_worker;
    std::promise<void> timeout_finished_signal;
    auto timeout_finished = timeout_finished_signal.get_future();
    std::atomic_bool timeout_cancelled{};
    std::atomic_bool timeout_aborted{};
    timeout_worker.spawn(
        [&timeout_finished_signal, &timeout_cancelled, &timeout_aborted](boost::asio::yield_context yield)
        {
            boost::asio::steady_timer operation_timer(yield.get_executor(), std::chrono::hours{1});
            boost::asio::steady_timer cancellation_timer(yield.get_executor());
            boost::system::error_code error;
            operation_timer.async_wait(boost::asio::cancel_after(cancellation_timer, std::chrono::milliseconds{1}, yield[error]));
            timeout_aborted.store(error == boost::asio::error::operation_aborted, std::memory_order_release);
            timeout_cancelled.store(yield.cancelled() != boost::asio::cancellation_type::none, std::memory_order_release);
            timeout_finished_signal.set_value();
        });

    std::promise<void> timeout_returned_signal;
    auto timeout_returned = timeout_returned_signal.get_future();
    std::jthread timeout_runner([&]() {
        timeout_worker.run();
        timeout_returned_signal.set_value();
    });
    timeout_finished.get();
    timeout_worker.request_stop();
    require(timeout_returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready, "timeout worker drains after local timeout");
    timeout_runner.join();
    require(timeout_aborted.load(std::memory_order_acquire), "cancel_after reports operation aborted");
    require(!timeout_cancelled.load(std::memory_order_acquire), "cancel_after does not mark worker cancellation state");
}

void test_emergency_stop_destroys_live_subscription()
{
    struct subscription_owner
    {
        worker_context::shutdown_subscription subscription;
    };

    for (std::size_t iteration = 0; iteration < 100U; ++iteration)
    {
        auto worker = std::make_unique<worker_context>();
        auto owner = std::make_shared<subscription_owner>();
        std::promise<void> registered_signal;
        auto registered = registered_signal.get_future();
        boost::asio::post(
            worker->io(),
            [&]()
            {
                owner->subscription = worker->subscribe_shutdown([owner]() {});
                require(static_cast<bool>(owner->subscription), "emergency stop subscription registered");
                registered_signal.set_value();
            });
        std::jthread runner([&]() { worker->run(); });
        registered.get();
        owner.reset();
        worker->stop();
        runner.join();
        worker.reset();
    }
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_request_stop_drains_spawned_operations();
        media_server::test_request_stop_runs_callback_shutdown_on_worker();
        media_server::test_pool_request_stop_drains_each_worker();
        media_server::test_cancellation_state_distinguishes_worker_stop_from_timeout();
        media_server::test_emergency_stop_destroys_live_subscription();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
