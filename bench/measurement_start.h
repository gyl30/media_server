#ifndef MEDIA_SERVER_BENCH_MEASUREMENT_START_H
#define MEDIA_SERVER_BENCH_MEASUREMENT_START_H

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace media_server::bench
{

inline std::optional<std::chrono::steady_clock::time_point> read_measurement_start(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
    {
        return std::nullopt;
    }
    std::int64_t nanoseconds{};
    if (!(input >> nanoseconds) || nanoseconds <= 0)
    {
        throw std::runtime_error("start-file requires positive CLOCK_MONOTONIC nanoseconds");
    }
    return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanoseconds}};
}

inline boost::asio::awaitable<std::chrono::steady_clock::time_point> wait_measurement_start(
    boost::asio::io_context& io, const std::string& path, std::chrono::seconds warmup, std::chrono::seconds timeout)
{
    auto start = std::chrono::steady_clock::now() + warmup;
    const auto deadline = start + timeout;
    boost::asio::steady_timer timer(io);
    if (!path.empty())
    {
        while (true)
        {
            const auto value = read_measurement_start(path);
            if (value)
            {
                start = *value;
                if (start < std::chrono::steady_clock::now() || start > deadline)
                {
                    throw std::runtime_error("start-file time is outside the readiness deadline");
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error("start-file timed out");
            }
            timer.expires_after(std::chrono::milliseconds{20});
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
    timer.expires_at(start);
    co_await timer.async_wait(boost::asio::use_awaitable);
    co_return start;
}

}    // namespace media_server::bench

#endif
