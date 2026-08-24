#include <array>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

extern "C"
{
#include "rtp.h"
#include "rtcp-header.h"
#include "rtp-demuxer.h"
#include "rtp-internal.h"
}

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void write_u32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t read_u32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) | (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

struct rtcp_capture
{
    std::vector<int> types;
    std::vector<std::uint32_t> ssrcs;
};

void capture_rtcp(void* param, const rtcp_msg_t* message)
{
    auto& capture = *static_cast<rtcp_capture*>(param);
    capture.types.push_back(message->type);
    capture.ssrcs.push_back(message->ssrc);
}

int ignore_payload(void*, const void*, int, std::uint32_t, int) { return 0; }

void test_rtcp_unpack_regressions()
{
    rtcp_capture capture;
    rtp_event_t handler{};
    handler.on_rtcp = &capture_rtcp;
    auto* rtp = static_cast<rtp_context*>(rtp_create(&handler, &capture, 0x10203040U, 0, 90'000, 2 * 1024 * 1024, 0));
    require(rtp != nullptr, "rtp context create");

    rtcp_header_t header{};
    header.v = 2;
    header.pt = RTCP_BYE;
    header.rc = 2;
    header.length = 2;
    std::array<std::uint8_t, 8> bye{};
    write_u32(bye.data(), 0x11223344U);
    write_u32(bye.data() + 4, 0x55667788U);
    rtcp_bye_unpack(rtp, &header, bye.data(), bye.size());
    require(capture.types == std::vector<int>({RTCP_BYE, RTCP_BYE}), "bye callback count and type");
    require(capture.ssrcs == std::vector<std::uint32_t>({0x11223344U, 0x55667788U}), "bye ssrc sequence");

    capture = {};
    header.pt = RTCP_SR;
    header.rc = 1;
    header.length = 12;
    std::array<std::uint8_t, 48> sr{};
    write_u32(sr.data(), 0x11223344U);
    write_u32(sr.data() + 4, 0x01020304U);
    write_u32(sr.data() + 8, 0x05060708U);
    write_u32(sr.data() + 24, 0x10203040U);
    rtcp_sr_unpack(rtp, &header, sr.data(), sr.size());
    require(capture.types == std::vector<int>({RTCP_SR}), "sr callback type");

    capture = {};
    header.pt = RTCP_SDES;
    header.rc = 1;
    header.length = 2;
    std::array<std::uint8_t, 8> sdes{};
    write_u32(sdes.data(), 0x11223344U);
    sdes[4] = RTCP_SDES_CNAME;
    sdes[5] = 1;
    sdes[6] = 'x';
    rtcp_sdes_unpack(rtp, &header, sdes.data(), sdes.size());
    require(capture.types == std::vector<int>({RTCP_SDES}), "sdes callback type");

    rtp_destroy(rtp);
}

void test_rtp_demuxer_receiver_report()
{
#if !defined(OS_WINDOWS)
    srand48(0);
#endif
    auto* demuxer = rtp_demuxer_create(0, 8'000, 0, "PCMU", &ignore_payload, nullptr);
    require(demuxer != nullptr, "rtp demuxer create");
    require(rtp_demuxer_set_info(demuxer, "receiver-cname", "media_server") == 0, "rtp demuxer set info");

    std::array<std::uint8_t, 13> rtp_packet{0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    require(rtp_demuxer_input(demuxer, rtp_packet.data(), static_cast<int>(rtp_packet.size())) == 0, "rtp demuxer input first rtp");
    rtp_packet[3] = 0x02;
    rtp_packet[7] = 0xa0;
    require(rtp_demuxer_input(demuxer, rtp_packet.data(), static_cast<int>(rtp_packet.size())) == 0, "rtp demuxer input second rtp");

    std::array<std::uint8_t, 8> minimal_rr{0x80, RTCP_RR, 0x00, 0x01, 0x55, 0x66, 0x77, 0x88};
    require(rtp_demuxer_input(demuxer, minimal_rr.data(), static_cast<int>(minimal_rr.size())) == RTCP_RR, "rtp demuxer accepts eight byte rtcp");

    std::array<std::uint8_t, 28> sender_report{};
    sender_report[0] = 0x80;
    sender_report[1] = RTCP_SR;
    sender_report[3] = 0x06;
    write_u32(sender_report.data() + 4, 0x11223344U);
    write_u32(sender_report.data() + 8, 0x01020304U);
    write_u32(sender_report.data() + 12, 0x05060708U);
    require(rtp_demuxer_input(demuxer, sender_report.data(), static_cast<int>(sender_report.size())) == RTCP_SR, "rtp demuxer input sender report");

    std::array<std::uint8_t, 1500> report{};
    int bytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (bytes == 0 && std::chrono::steady_clock::now() < deadline)
    {
        bytes = rtp_demuxer_rtcp(demuxer, report.data(), static_cast<int>(report.size()));
        if (bytes == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    require(bytes > 0, "rtp demuxer receiver report due");
    require(report[1] == RTCP_RR, "rtp demuxer receiver report type");
    const auto local_ssrc = read_u32(report.data() + 4);
    require(local_ssrc != 0, "rtp demuxer receiver ssrc nonzero");
    require((report[0] & 0x1fU) == 1U, "rtp demuxer receiver report block count");
    require(read_u32(report.data() + 8) == 0x11223344U, "rtp demuxer receiver report source ssrc");
    require(read_u32(report.data() + 24) == 0x03040506U, "rtp demuxer receiver report lsr");

    const auto rr_size = static_cast<std::size_t>((static_cast<std::uint16_t>(report[2]) << 8U) | report[3] | 0U) * 4U + 4U;
    require(rr_size + 10U < static_cast<std::size_t>(bytes), "rtp demuxer compound sdes present");
    require(report[rr_size + 1U] == RTCP_SDES, "rtp demuxer sdes type");
    require(read_u32(report.data() + rr_size + 4U) == local_ssrc, "rtp demuxer sdes ssrc");
    require(report[rr_size + 8U] == RTCP_SDES_CNAME, "rtp demuxer cname item");
    const auto cname_size = static_cast<std::size_t>(report[rr_size + 9U]);
    require(std::string_view(reinterpret_cast<const char*>(report.data() + rr_size + 10U), cname_size) == "receiver-cname",
            "rtp demuxer cname value");

    rtp_demuxer_destroy(&demuxer);
}

void test_rtp_sender_report_statistics()
{
    constexpr std::uint32_t ssrc = 0x10203040U;
    rtp_event_t handler{};
    auto* sender = rtp_create(&handler, nullptr, ssrc, 0, 90'000, 2 * 1024 * 1024, 1);
    require(sender != nullptr, "rtp sender create");

    std::vector<std::uint8_t> first(112U, 0x11U);
    first[0] = 0x80U;
    first[1] = 96U;
    first[2] = 0x00U;
    first[3] = 0x01U;
    write_u32(first.data() + 4, 9'000U);
    write_u32(first.data() + 8, ssrc);
    require(rtp_onsend(sender, first.data(), static_cast<int>(first.size())) == 0, "rtp sender first packet");

    std::vector<std::uint8_t> second(212U, 0x22U);
    second[0] = 0x80U;
    second[1] = 96U;
    second[2] = 0x00U;
    second[3] = 0x02U;
    write_u32(second.data() + 4, 18'000U);
    write_u32(second.data() + 8, ssrc);
    require(rtp_onsend(sender, second.data(), static_cast<int>(second.size())) == 0, "rtp sender second packet");

    std::array<std::uint8_t, 1500> first_report{};
    const auto first_bytes = rtp_rtcp_report(sender, first_report.data(), static_cast<int>(first_report.size()));
    require(first_bytes >= 28 && first_report[1] == RTCP_SR, "rtp sender first report");
    require(read_u32(first_report.data() + 4) == ssrc, "rtp sender report ssrc");
    require(read_u32(first_report.data() + 8) != 0, "rtp sender report ntp");
    require(read_u32(first_report.data() + 16) >= 18'000U, "rtp sender report timestamp");
    require(read_u32(first_report.data() + 20) == 2U, "rtp sender report packet count");
    require(read_u32(first_report.data() + 24) == 300U, "rtp sender report payload octets");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::array<std::uint8_t, 1500> second_report{};
    const auto second_bytes = rtp_rtcp_report(sender, second_report.data(), static_cast<int>(second_report.size()));
    require(second_bytes >= 28 && second_report[1] == RTCP_SR, "rtp sender second report");
    const auto first_ntp = (static_cast<std::uint64_t>(read_u32(first_report.data() + 8)) << 32U) | read_u32(first_report.data() + 12);
    const auto second_ntp = (static_cast<std::uint64_t>(read_u32(second_report.data() + 8)) << 32U) | read_u32(second_report.data() + 12);
    require(second_ntp > first_ntp, "rtp sender report ntp progression");
    require(read_u32(second_report.data() + 16) > read_u32(first_report.data() + 16), "rtp sender report timestamp progression");
    require(read_u32(second_report.data() + 20) == 2U && read_u32(second_report.data() + 24) == 300U, "rtp sender report stable counters");

    rtp_destroy(sender);
}

}    // namespace

int main()
{
    try
    {
        test_rtcp_unpack_regressions();
        std::cout << "[pass] rtcp_unpack_regressions\n";
        test_rtp_demuxer_receiver_report();
        std::cout << "[pass] rtp_demuxer_receiver_report\n";
        test_rtp_sender_report_statistics();
        std::cout << "[pass] rtp_sender_report_statistics\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
