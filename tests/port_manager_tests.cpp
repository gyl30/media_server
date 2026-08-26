#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "media/net/port_manager.h"

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

void test_single_reservation()
{
    port_manager::init(100, 103);
    auto& manager = port_manager::instance();
    const auto first = manager.acquire();
    const auto second = manager.acquire();
    require(first && second && *first != *second, "single reservations are unique");
    manager.release(*first);
    const auto reused = manager.acquire();
    require(reused && *reused == *first, "released single port is reusable");
    manager.release(*second);
    manager.release(*reused);
    manager.release(*reused);
    port_manager::destroy();
}

void test_pair_reservation()
{
    port_manager::init(200, 205);
    auto& manager = port_manager::instance();
    const auto pair = manager.acquire_pair();
    require(pair && (pair->first % 2U) == 0U && pair->second == pair->first + 1U, "pair is adjacent even odd");
    const auto single = manager.acquire();
    require(single && *single != pair->first && *single != pair->second, "pair ports are reserved atomically");
    manager.release(*single);
    manager.release(*pair);
    const auto reused = manager.acquire_pair();
    require(reused && reused->first == pair->first && reused->second == pair->second, "released pair is reusable");
    manager.release(*reused);
    port_manager::destroy();
}

void test_exhaustion()
{
    port_manager::init(300, 301);
    auto& manager = port_manager::instance();
    require(manager.acquire() && manager.acquire(), "single range fills");
    require(!manager.acquire(), "single range exhaustion");
    port_manager::destroy();

    port_manager::init(302, 303);
    auto& pair_manager = port_manager::instance();
    require(pair_manager.acquire_pair().has_value(), "pair range fills");
    require(!pair_manager.acquire_pair(), "pair range exhaustion");
    port_manager::destroy();
}

void test_concurrent_reservation()
{
    port_manager::init(400, 799);
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t reservations_per_thread = 32;
    std::mutex mutex;
    std::vector<std::uint16_t> reservations;
    reservations.reserve(thread_count * reservations_per_thread);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index)
    {
        threads.emplace_back([&]
        {
            std::vector<std::uint16_t> local;
            local.reserve(reservations_per_thread);
            for (std::size_t count = 0; count < reservations_per_thread; ++count)
            {
                const auto port = port_manager::instance().acquire();
                require(port.has_value(), "concurrent reservation available");
                local.push_back(*port);
            }
            std::scoped_lock lock(mutex);
            reservations.insert(reservations.end(), local.begin(), local.end());
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    const std::set<std::uint16_t> unique(reservations.begin(), reservations.end());
    require(unique.size() == reservations.size(), "concurrent reservations are unique");
    for (const auto port : reservations)
    {
        port_manager::instance().release(port);
    }
    port_manager::destroy();
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_single_reservation();
        media_server::test_pair_reservation();
        media_server::test_exhaustion();
        media_server::test_concurrent_reservation();
        std::cout << "[pass] port_manager_tests\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] port_manager_tests: " << error.what() << '\n';
        return 1;
    }
}
