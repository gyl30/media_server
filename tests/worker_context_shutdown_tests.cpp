#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

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
    require(worker.active_task_count() == 0U, "request_stop drains tracked tasks");
    require(aborted.load(std::memory_order_acquire) == 3U, "request_stop cancels accept read and timer");
    require(completion_threads.size() == 3U, "all canceled coroutines resumed");
    for (const auto completion_thread : completion_threads)
    {
        require(completion_thread == worker_thread, "coroutine completes on worker thread");
    }
    require(!spawned_after_stop.load(std::memory_order_acquire), "request_stop rejects new coroutine");
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_request_stop_drains_spawned_operations();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
