#ifndef MEDIA_WEBRTC_RTCP_RECEIVER_H
#define MEDIA_WEBRTC_RTCP_RECEIVER_H

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct rtcp_msg_t;

namespace media_server
{

struct rtcp_receiver_report
{
    std::uint32_t sender_ssrc{};
    std::uint32_t source_ssrc{};
    std::uint8_t fraction_lost{};
    std::int32_t cumulative_lost{};
    std::uint32_t highest_sequence{};
    std::uint32_t jitter{};
    std::uint32_t lsr{};
    std::uint32_t dlsr{};
};

struct rtcp_pli
{
    std::uint32_t sender_ssrc{};
    std::uint32_t media_ssrc{};
};

struct rtcp_receive_result
{
    std::vector<rtcp_receiver_report> receiver_reports;
    std::vector<rtcp_pli> plis;
};

class rtcp_receiver final
{
   public:
    rtcp_receiver();

    [[nodiscard]] bool input(std::span<const std::uint8_t> packet, rtcp_receive_result& result);

   private:
    struct context_deleter
    {
        void operator()(void* value) const noexcept;
    };

    static void on_rtcp(void* param, const ::rtcp_msg_t* message);
    void handle_rtcp(const ::rtcp_msg_t& message);

    std::unique_ptr<void, context_deleter> context_;
    rtcp_receive_result* result_{};
};

}    // namespace media_server

#endif
