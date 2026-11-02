#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/webrtc/dtls_certificate.h"

static std::atomic<int> created{0};

static void make_one()
{
    if (media_server::dtls_certificate::create())
    {
        created.fetch_add(1);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s threads|serial|fibers N [workers]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const int count = std::atoi(argv[2]);
    if (mode == "serial")
    {
        for (int i = 0; i < count; ++i)
        {
            make_one();
        }
    }
    else if (mode == "threads")
    {
        std::latch start(count);
        std::vector<std::thread> threads;
        for (int i = 0; i < count; ++i)
        {
            threads.emplace_back([&start]() { start.arrive_and_wait(); make_one(); });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    }
    else if (mode == "fibers")
    {
        const int workers = argc > 3 ? std::atoi(argv[3]) : 1;
        const auto start_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        std::vector<std::unique_ptr<boost::asio::io_context>> contexts;
        for (int w = 0; w < workers; ++w)
        {
            contexts.push_back(std::make_unique<boost::asio::io_context>(1));
        }
        for (int i = 0; i < count; ++i)
        {
            auto& io = *contexts[i % workers];
            boost::asio::spawn(io,
                               [&io, start_at](boost::asio::yield_context yield)
                               {
                                   boost::asio::steady_timer timer(io, start_at);
                                   timer.async_wait(yield);
                                   make_one();
                               },
                               [](std::exception_ptr) {});
        }
        std::vector<std::thread> threads;
        for (auto& io : contexts)
        {
            threads.emplace_back([&io]() { io->run(); });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    }
    else
    {
        return 2;
    }
    std::printf("created %d/%d\n", created.load(), count);
    return created.load() == count ? 0 : 1;
}
