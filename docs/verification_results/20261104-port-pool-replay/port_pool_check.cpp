#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <string>
#include <cstdint>

#include "media/net/media_port_pool.h"

namespace
{

using media_server::media_port_pool;

int failures = 0;

void expect(bool condition, const std::string& message)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    failures += condition ? 0 : 1;
}

}    // namespace

int main()
{
    media_port_pool::init();
    auto& pool = media_port_pool::instance();

    const auto first = pool.acquire();
    expect(first == 49'152, "first allocation is 49152");
    pool.release(*first);
    expect(pool.acquire() == first, "released port is reused");
    pool.release(*first);

    std::vector<std::uint16_t> ports;
    bool shape = true;
    while (const auto port = pool.acquire())
    {
        shape = shape && *port % 2 == 0 && *port >= 49'152 && *port + 1 <= 65'535;
        ports.push_back(*port);
    }
    expect(ports.size() == 8'192, "8192 RTP/RTCP pairs before exhaustion, got " + std::to_string(ports.size()));
    expect(shape, "every RTP port is even and its RTCP port is in range");
    expect(std::set<std::uint16_t>(ports.begin(), ports.end()).size() == ports.size(), "no port is handed out twice");
    expect(!pool.acquire().has_value(), "exhausted pool returns nullopt");

    pool.release(ports[100]);
    pool.release(ports[100]);
    pool.release(49'153);
    pool.release(1'000);
    expect(pool.acquire() == ports[100], "after one release exactly that port is available");
    expect(!pool.acquire().has_value(), "double release and unknown ports do not add capacity");

    for (const auto port : ports)
    {
        pool.release(port);
    }

    std::mutex mutex;
    std::set<std::uint16_t> held;
    bool unique = true;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread)
    {
        threads.emplace_back(
            [&]()
            {
                for (int round = 0; round < 20'000; ++round)
                {
                    const auto port = pool.acquire();
                    if (!port)
                    {
                        continue;
                    }
                    {
                        std::scoped_lock lock(mutex);
                        unique = unique && held.insert(*port).second;
                    }
                    {
                        std::scoped_lock lock(mutex);
                        held.erase(*port);
                    }
                    pool.release(*port);
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    expect(unique, "8 threads acquiring and releasing never hold the same port");
    std::size_t total = 0;
    while (pool.acquire())
    {
        ++total;
    }
    expect(total == 8'192, "all 8192 pairs available after concurrent churn, got " + std::to_string(total));

    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
