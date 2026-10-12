#ifndef MEDIA_SERVER_BENCH_PACKET_HEALTH_H
#define MEDIA_SERVER_BENCH_PACKET_HEALTH_H

#include <cstdint>
#include <optional>
#include <span>

namespace media_server::bench
{

class timestamp_health
{
   public:
    bool observe(std::uint32_t timestamp)
    {
        if (last_ && timestamp - *last_ > 0x80000000U)
        {
            return false;
        }
        last_ = timestamp;
        return true;
    }

   private:
    std::optional<std::uint32_t> last_;
};

class rtp_health
{
   public:
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
            timestamp_ = {};
        }
        if (sequence_ && static_cast<std::uint16_t>(sequence - *sequence_) != 1)
        {
            return false;
        }
        sequence_ = sequence;
        return timestamp_.observe(read_u32(packet.subspan(4)));
    }

   private:
    static std::uint32_t read_u32(std::span<const std::uint8_t> bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
    }

    std::optional<std::uint32_t> ssrc_;
    std::optional<std::uint16_t> sequence_;
    timestamp_health timestamp_;
};

}    // namespace media_server::bench

#endif
