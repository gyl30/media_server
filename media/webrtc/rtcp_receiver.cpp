#include "media/webrtc/rtcp_receiver.h"

extern "C"
{
#include "rtp.h"
}

#include <climits>
#include <cstddef>

namespace media_server
{
namespace
{

constexpr std::uint32_t parser_ssrc = 0x52544350U;
constexpr int parser_clock_rate = 90'000;
constexpr int parser_bandwidth = 1;

std::int32_t signed_cumulative_lost(std::uint32_t cumulative)
{
    const auto value = cumulative & 0x00FF'FFFFU;
    if ((value & 0x0080'0000U) == 0U)
    {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(value) - 0x0100'0000;
}

bool validate_rtcp(std::span<const std::uint8_t> packet)
{
    if (packet.empty())
    {
        return false;
    }

    std::size_t offset = 0;
    while (offset < packet.size())
    {
        const auto remaining = packet.size() - offset;
        if (remaining < 4U)
        {
            return false;
        }

        const auto first = packet[offset];
        const auto version = static_cast<std::uint8_t>(first >> 6U);
        const bool padding = (first & 0x20U) != 0U;
        const auto format = static_cast<std::uint8_t>(first & 0x1FU);
        const auto packet_type = packet[offset + 1U];
        const auto length_words = (static_cast<std::size_t>(packet[offset + 2U]) << 8U) | static_cast<std::size_t>(packet[offset + 3U]);
        const auto packet_size = (length_words + 1U) * 4U;

        if (version != 2U || packet_type < RTCP_SR || packet_type > RTCP_XR || packet_size < 4U || packet_size > remaining)
        {
            return false;
        }

        const bool last_packet = packet_size == remaining;
        std::size_t unpadded_size = packet_size;
        if (padding)
        {
            if (!last_packet)
            {
                return false;
            }

            const auto padding_size = static_cast<std::size_t>(packet[offset + packet_size - 1U]);
            if (padding_size == 0U || padding_size > packet_size - 4U)
            {
                return false;
            }
            unpadded_size -= padding_size;
        }

        if (packet_type == RTCP_RR)
        {
            const auto expected_size = 8U + static_cast<std::size_t>(format) * 24U;
            if (unpadded_size != expected_size)
            {
                return false;
            }
        }

        if (packet_type == RTCP_PSFB && format == RTCP_PSFB_PLI)
        {
            if (padding || packet_size != 12U)
            {
                return false;
            }
        }

        offset += packet_size;
    }

    return offset == packet.size();
}

}    // namespace

rtcp_receiver::rtcp_receiver()
{
    rtp_event_t handler{};
    handler.on_rtcp = &rtcp_receiver::on_rtcp;
    context_.reset(rtp_create(&handler, this, parser_ssrc, 0, parser_clock_rate, parser_bandwidth, 1));
}

bool rtcp_receiver::input(std::span<const std::uint8_t> packet, rtcp_receive_result& result)
{
    result.receiver_reports.clear();
    result.plis.clear();

    if (!context_ || packet.empty() || packet.size() > static_cast<std::size_t>(INT_MAX) || !validate_rtcp(packet))
    {
        return false;
    }

    result_ = &result;
    const auto status = rtp_onreceived_rtcp(context_.get(), packet.data(), static_cast<int>(packet.size()));
    result_ = nullptr;
    return status >= 0;
}

void rtcp_receiver::context_deleter::operator()(void* value) const noexcept
{
    if (value != nullptr)
    {
        static_cast<void>(rtp_destroy(value));
    }
}

void rtcp_receiver::on_rtcp(void* param, const ::rtcp_msg_t* message)
{
    if (param == nullptr || message == nullptr)
    {
        return;
    }
    static_cast<rtcp_receiver*>(param)->handle_rtcp(*message);
}

void rtcp_receiver::handle_rtcp(const ::rtcp_msg_t& message)
{
    if (result_ == nullptr)
    {
        return;
    }

    if (message.type == RTCP_RR)
    {
        result_->receiver_reports.push_back(rtcp_receiver_report{
            .sender_ssrc = message.ssrc,
            .source_ssrc = message.u.rr.ssrc,
            .fraction_lost = static_cast<std::uint8_t>(message.u.rr.fraction),
            .cumulative_lost = signed_cumulative_lost(message.u.rr.cumulative),
            .highest_sequence = message.u.rr.exthsn,
            .jitter = message.u.rr.jitter,
            .lsr = message.u.rr.lsr,
            .dlsr = message.u.rr.dlsr,
        });
        return;
    }

    if (message.type == (RTCP_PSFB | (RTCP_PSFB_PLI << 8)))
    {
        result_->plis.push_back(rtcp_pli{
            .sender_ssrc = message.ssrc,
            .media_ssrc = message.u.psfb.media,
        });
    }
}

}    // namespace media_server
