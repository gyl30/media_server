#ifndef MEDIA_RTMP_RTMP_TIMESTAMP_H
#define MEDIA_RTMP_RTMP_TIMESTAMP_H

#include <cstdint>

namespace media_server
{

struct rtmp_timestamp_state
{
    std::uint32_t last{};
    std::int64_t milliseconds{};
    bool started{};
};

[[nodiscard]] constexpr std::int64_t rtmp_timestamp_delta(std::uint32_t current, std::uint32_t previous) noexcept
{
    constexpr std::int64_t cycle = std::int64_t{1} << 32;
    constexpr std::uint32_t half_cycle = std::uint32_t{1} << 31;
    const auto delta = current - previous;
    return delta < half_cycle ? static_cast<std::int64_t>(delta) : static_cast<std::int64_t>(delta) - cycle;
}

[[nodiscard]] inline std::int64_t unwrap_rtmp_timestamp(std::uint32_t timestamp, rtmp_timestamp_state& state) noexcept
{
    if (!state.started)
    {
        state.last = timestamp;
        state.milliseconds = timestamp;
        state.started = true;
        return state.milliseconds;
    }

    state.milliseconds += rtmp_timestamp_delta(timestamp, state.last);
    state.last = timestamp;
    return state.milliseconds;
}

}    // namespace media_server

#endif
