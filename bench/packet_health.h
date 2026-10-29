#ifndef MEDIA_SERVER_BENCH_PACKET_HEALTH_H
#define MEDIA_SERVER_BENCH_PACKET_HEALTH_H

#include <span>
#include <cstdint>
#include <optional>

namespace media_server::bench
{

constexpr std::uint32_t video_reorder_ticks = 90'000;

class timestamp_health
{
   public:
    explicit timestamp_health(std::uint32_t reorder_window = 0) : reorder_window_(reorder_window) {}

    bool observe(std::uint32_t timestamp)
    {
        if (latest_)
        {
            const auto forward = timestamp - *latest_;
            if (forward > 0x80000000U)
            {
                return *latest_ - timestamp <= reorder_window_;
            }
            advanced_ += forward;
        }
        latest_ = timestamp;
        return true;
    }

    [[nodiscard]] std::uint64_t advanced() const noexcept { return advanced_; }

   private:
    std::uint32_t reorder_window_;
    std::optional<std::uint32_t> latest_;
    std::uint64_t advanced_{};
};

class rtp_health
{
   public:
    explicit rtp_health(std::uint32_t reorder_window = 0) : reorder_window_(reorder_window), timestamp_(reorder_window) {}

    bool observe(std::span<const std::uint8_t> packet)
    {
        if (packet.size() < 12 || packet[0] >> 6U != 2)
        {
            return false;
        }
        const auto sequence = static_cast<std::uint16_t>((static_cast<std::uint16_t>(packet[2]) << 8U) | packet[3]);
        const auto ssrc = read_u32(packet.subspan(8));
        if (!ssrc_ || *ssrc_ != ssrc)
        {
            ssrc_ = ssrc;
            sequence_.reset();
            previous_advanced_ += timestamp_.advanced();
            timestamp_ = timestamp_health(reorder_window_);
        }
        if (sequence_ && static_cast<std::uint16_t>(sequence - *sequence_) != 1)
        {
            return false;
        }
        sequence_ = sequence;
        return timestamp_.observe(read_u32(packet.subspan(4)));
    }

    [[nodiscard]] std::uint64_t advanced() const noexcept { return previous_advanced_ + timestamp_.advanced(); }

   private:
    static std::uint32_t read_u32(std::span<const std::uint8_t> bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
    }

    std::uint32_t reorder_window_;
    std::optional<std::uint32_t> ssrc_;
    std::optional<std::uint16_t> sequence_;
    timestamp_health timestamp_;
    std::uint64_t previous_advanced_{};
};

}    // namespace media_server::bench

#endif
