#include <span>
#include <array>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "media/net/worker_context.h"
#include "media/net/media_port_pool.h"
#include "media/gb28181/gb28181_audio_sender.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using udp = boost::asio::ip::udp;
const auto loopback = boost::asio::ip::address_v4::loopback();
std::uint16_t pool_port{};
constexpr std::uint32_t media_ssrc = 0x12345678U;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void on_owner(worker_context& worker, Function function)
{
    bool completed{};
    boost::asio::post(worker.io(),
                      [&]()
                      {
                          function();
                          completed = true;
                      });
    worker.io().poll();
    require(completed, "owner operation did not run");
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) | bytes[offset + 3U];
}

bool would_block(boost::system::error_code error) { return error == boost::asio::error::would_block || error == boost::asio::error::try_again; }

struct datagram
{
    std::vector<std::uint8_t> bytes;
    udp::endpoint peer;
};

datagram receive(worker_context& worker, udp::socket& socket, std::chrono::milliseconds timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint peer;
    do
    {
        boost::system::error_code error;
        const auto bytes = socket.receive_from(boost::asio::buffer(buffer), peer, 0, error);
        if (!error)
        {
            return {{buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes)}, peer};
        }
        require(would_block(error), "loopback receive failed");
        worker.io().run_one_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("loopback datagram deadline exceeded");
}

void require_empty(udp::socket& socket)
{
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint peer;
    boost::system::error_code error;
    socket.receive_from(boost::asio::buffer(buffer), peer, 0, error);
    require(would_block(error), "unexpected datagram before peer configuration");
}

void discard(udp::socket& socket)
{
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint peer;
    for (;;)
    {
        boost::system::error_code error;
        socket.receive_from(boost::asio::buffer(buffer), peer, 0, error);
        if (would_block(error))
        {
            return;
        }
        require(!error, "discard receive failed");
    }
}

void require_port_pair_released(worker_context& worker)
{
    const auto allocation = media_port_pool::instance().acquire();
    require(allocation == pool_port && !media_port_pool::instance().acquire(), "port pair leaked or was returned twice");
    udp::socket rtp(worker.io(), {loopback, pool_port});
    udp::socket rtcp(worker.io(), {loopback, static_cast<std::uint16_t>(pool_port + 1U)});
    media_port_pool::instance().release(*allocation);
}

struct fixture
{
    worker_context worker;
    udp::socket rtp{worker.io(), {loopback, 0}};
    udp::socket rtcp{worker.io(), {loopback, 0}};
    unsigned errors{};
    boost::system::error_code last_error;
    std::shared_ptr<gb28181_audio_sender> sender;

    fixture()
        : sender(std::make_shared<gb28181_audio_sender>(worker,
                                                        [this](boost::system::error_code error)
                                                        {
                                                            require(static_cast<bool>(error), "successful completion reported as sender error");
                                                            ++errors;
                                                            last_error = error;
                                                        }))
    {
        rtp.non_blocking(true);
        rtcp.non_blocking(true);
    }

    ~fixture()
    {
        if (sender)
        {
            sender->shutdown();
        }
        worker.request_stop();
        worker.io().run_for(1s);
    }

    void configure() { require(sender->set_peer(rtp.local_endpoint(), rtcp.local_endpoint(), media_ssrc), "peer configuration failed"); }

    void close()
    {
        const std::weak_ptr<gb28181_audio_sender> lifetime = sender;
        sender->shutdown();
        sender->shutdown();
        sender.reset();
        worker.io().run_for(50ms);
        require(lifetime.expired(), "closed sender retained by callbacks or outstanding operations");
        require(errors == 0, "explicit shutdown emitted an error callback");
        require_port_pair_released(worker);
    }
};

void check_sender_report(const datagram& report, std::uint32_t packets)
{
    const auto bytes = std::span<const std::uint8_t>{report.bytes};
    require(report.peer == udp::endpoint(loopback, static_cast<std::uint16_t>(pool_port + 1U)), "RTCP source is not its bound pair port");
    require(bytes.size() >= 28U && bytes[0] == 0x80 && bytes[1] == 200, "missing RTCP sender report");
    require(read_u32(bytes, 4) == media_ssrc && read_u32(bytes, 8) != 0, "RTCP sender identity/NTP corrupted");
    require(read_u32(bytes, 20) == packets && read_u32(bytes, 24) == packets * 160U, "RTCP counted invalid or dropped audio");
    std::size_t offset{};
    bool found_cname{};
    while (offset < bytes.size())
    {
        require(bytes.size() - offset >= 4U && (bytes[offset] >> 6U) == 2U, "malformed compound RTCP header");
        const auto size = (static_cast<std::size_t>(read_u16(bytes, offset + 2U)) + 1U) * 4U;
        require(size <= bytes.size() - offset, "compound RTCP length exceeds datagram");
        if (bytes[offset + 1U] == 202)
        {
            require(size >= 8U && (bytes[offset] & 0x1fU) == 1U, "invalid sender SDES chunk count");
            require(read_u32(bytes, offset + 4U) == media_ssrc, "SDES SSRC differs from RTP/SR");
            auto cursor = offset + 8U;
            const auto end = offset + size;
            while (cursor < end)
            {
                const auto type = bytes[cursor++];
                if (type == 0)
                {
                    break;
                }
                require(cursor < end, "truncated SDES item length");
                const auto length = static_cast<std::size_t>(bytes[cursor++]);
                require(length <= end - cursor, "SDES item exceeds its RTCP packet");
                if (type == 1)
                {
                    const std::string cname(reinterpret_cast<const char*>(bytes.data() + cursor), length);
                    require(cname == std::to_string(media_ssrc) + "@media_server", "SDES CNAME content corrupted");
                    found_cname = true;
                }
                cursor += length;
            }
        }
        offset += size;
    }
    require(found_cname, "RTCP SR was not accompanied by SDES CNAME");
}

void media_and_sender_report()
{
    fixture live;
    std::array<std::uint8_t, 160> samples{};
    on_owner(live.worker,
             [&]()
             {
                 require(live.sender->startup(loopback), "audio startup failed");
                 require(live.sender->local_port() == pool_port, "startup did not expose the reserved port");
                 require(!media_port_pool::instance().acquire(), "startup did not reserve its port pair");
                 for (unsigned offset = 0; offset < 2; ++offset)
                 {
                     udp::socket probe(live.worker.io());
                     probe.open(udp::v4());
                     boost::system::error_code error;
                     probe.bind({loopback, static_cast<std::uint16_t>(pool_port + offset)}, error);
                     require(error == boost::asio::error::address_in_use, "SIP-advertised port was not bound before peer configuration");
                 }

                 require(!live.sender->input_audio(samples), "unconfigured audio accepted");
             });
    live.worker.io().run_for(20ms);
    require_empty(live.rtp);
    require_empty(live.rtcp);

    std::chrono::steady_clock::time_point last_input;
    on_owner(live.worker,
             [&]()
             {
                 live.configure();
                 for (std::uint8_t frame = 0; frame < 4; ++frame)
                 {
                     std::array<std::uint8_t, 161> invalid{};
                     require(!live.sender->input_audio(invalid), "oversized input accepted");
                     require(!live.sender->input_audio(std::span{invalid.data(), std::size_t{159}}), "short input accepted");
                     require(!live.sender->input_audio({}), "empty input accepted");
                     samples.fill(frame);
                     require(live.sender->input_audio(samples), "valid PCMA rejected");
                     last_input = std::chrono::steady_clock::now();
                 }
             });
    std::uint16_t previous_sequence{};
    std::uint32_t previous_timestamp{};
    for (std::uint8_t frame = 0; frame < 4; ++frame)
    {
        const auto packet = receive(live.worker, live.rtp);
        const auto bytes = std::span<const std::uint8_t>{packet.bytes};
        require(packet.peer == udp::endpoint(loopback, pool_port), "RTP source differs from the prebound endpoint");
        require(bytes.size() == 172U && bytes[0] == 0x80 && (bytes[1] & 0x7fU) == 8, "PCMA RTP header/size corrupted");
        require(read_u32(bytes, 8) == media_ssrc, "PCMA RTP SSRC changed");
        const auto sequence = read_u16(bytes, 2);
        const auto timestamp = read_u32(bytes, 4);
        require(frame == 0 || sequence == static_cast<std::uint16_t>(previous_sequence + 1U), "RTP sequence order corrupted");
        require(frame == 0 || timestamp - previous_timestamp == 160U, "invalid input advanced RTP time or audio clock is not 8 kHz");
        for (std::size_t index = 12; index < bytes.size(); ++index)
        {
            require(bytes[index] == frame, "PCMA payload was changed or reordered");
        }
        previous_sequence = sequence;
        previous_timestamp = timestamp;
    }
    const auto report = receive(live.worker, live.rtcp, 7s);
    check_sender_report(report, 4);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_input).count();
    const auto extrapolated = static_cast<double>(read_u32(report.bytes, 16) - previous_timestamp);
    require(extrapolated > (elapsed - 1.0) * 8000.0 && extrapolated < (elapsed + 1.0) * 8000.0, "RTCP SR extrapolation uses the wrong audio clock");
    live.close();
    std::cout << "PCMA prebind, RTP payload/order/clock and real RTCP SR: PASS\n";
}

void overflow_recovers()
{
    fixture live;
    std::array<std::uint8_t, 160> samples{};
    std::uint32_t accepted{};
    unsigned dropped{};
    on_owner(live.worker,
             [&]()
             {
                 require(live.sender->startup(loopback), "overflow startup failed");
                 live.configure();
                 // 同一owner handler内不执行completion，确定性填满transport有界队列。
                 for (unsigned frame = 0; frame < 6110; ++frame)
                 {
                     if (live.sender->input_audio(samples))
                     {
                         ++accepted;
                     }
                     else
                     {
                         ++dropped;
                     }
                 }
                 require(accepted > 0 && dropped > 0 && live.errors == 0, "overflow not exercised or treated as a fatal socket error");
             });
    const auto first_packet = receive(live.worker, live.rtp);
    discard(live.rtp);
    samples.fill(0x7e);
    on_owner(live.worker, [&]() { require(live.sender->input_audio(samples), "audio did not recover after queue overflow"); });
    const auto packet = receive(live.worker, live.rtp);
    require(packet.bytes.size() == 172U && packet.bytes[12] == 0x7e, "recovered audio was not transmitted");
    require(read_u32(packet.bytes, 4) - read_u32(first_packet.bytes, 4) == 6110U * 160U, "dropped audio did not advance the sample clock");
    require(read_u16(packet.bytes, 2) == static_cast<std::uint16_t>(read_u16(first_packet.bytes, 2) + 6110U), "overflow reused RTP sequence numbers");
    check_sender_report(receive(live.worker, live.rtcp, 7s), accepted + 1U);
    live.close();
    std::cout << "PCMA queue overflow, recovery and RTCP accepted-packet accounting: PASS\n";
}

struct parent_owner
{
    std::shared_ptr<gb28181_audio_sender> sender;
    unsigned errors{};
};

void in_flight_close_breaks_parent_cycle()
{
    worker_context worker;
    udp::socket rtp(worker.io(), {loopback, 0});
    udp::socket rtcp(worker.io(), {loopback, 0});
    rtp.non_blocking(true);
    auto parent = std::make_shared<parent_owner>();
    parent->sender = std::make_shared<gb28181_audio_sender>(worker, [parent](boost::system::error_code) { ++parent->errors; });
    const std::weak_ptr<parent_owner> parent_lifetime = parent;
    const std::weak_ptr<gb28181_audio_sender> lifetime = parent->sender;
    on_owner(worker,
             [&]()
             {
                 require(parent->sender->startup(loopback), "in-flight startup failed");
                 require(parent->sender->set_peer(rtp.local_endpoint(), rtcp.local_endpoint(), media_ssrc), "in-flight configuration failed");
                 // 先排入cleanup，再模拟当前handler中已持有的迟到输入；首包仍可在cleanup前提交。
                 parent->sender->shutdown();
                 parent->sender->shutdown();
                 std::array<std::uint8_t, 160> samples{};
                 for (unsigned frame = 0; frame < 3; ++frame)
                 {
                     require(parent->sender->input_audio(samples), "in-flight enqueue failed");
                 }
                 // 首包已提交，其余只能排队。父子强引用只能由实际cleanup断开。
                 parent.reset();
                 require(!parent_lifetime.expired() && !lifetime.expired(), "posted cleanup lost its callback ownership");
             });
    worker.io().run_for(50ms);
    require(parent_lifetime.expired() && lifetime.expired(), "in-flight close retained the parent/child callback cycle");
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint peer;
    unsigned packets{};
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = rtp.receive_from(boost::asio::buffer(buffer), peer, 0, error);
        if (would_block(error))
        {
            break;
        }
        require(!error && bytes == 172U, "in-flight PCMA packet corrupted");
        ++packets;
    }
    require(packets <= 1, "queued RTP was sent after owner cleanup");
    require_port_pair_released(worker);
    worker.request_stop();
    worker.io().run_for(1s);
    require(worker.io().stopped(), "in-flight shutdown did not drain");
    std::cout << "PCMA in-flight/repeated close, parent callback release and port rebind: PASS\n";
}

void close_before_peer_and_worker_stop()
{
    {
        fixture live;
        on_owner(live.worker, [&]() { require(live.sender->startup(loopback), "pre-peer startup failed"); });
        live.sender->shutdown();
        live.worker.io().poll();
        on_owner(live.worker,
                 [&]()
                 {
                     std::array<std::uint8_t, 160> samples{};
                     require(!live.sender->local_port() && !live.sender->input_audio(samples), "closed unconfigured sender accepted audio");
                     require(!live.sender->set_peer(live.rtp.local_endpoint(), live.rtcp.local_endpoint(), media_ssrc),
                             "closed sender configured a peer");
                 });
        live.close();
    }
    {
        fixture live;
        on_owner(live.worker,
                 [&]()
                 {
                     require(live.sender->startup(loopback), "worker-stop startup failed");
                     live.configure();
                     std::array<std::uint8_t, 160> samples{};
                     require(live.sender->input_audio(samples), "worker-stop enqueue failed");
                 });
        const std::weak_ptr<gb28181_audio_sender> lifetime = live.sender;
        live.sender.reset();
        live.worker.request_stop();
        live.worker.io().run_for(1s);
        require(lifetime.expired() && live.worker.io().stopped(), "worker stop retained the audio sender or RTCP read");
        require(live.errors == 0, "worker cancellation escaped as an unexpected media error");
        require_port_pair_released(live.worker);
    }
    std::cout << "PCMA close before peer configuration and worker-stop cleanup: PASS\n";
}

void partial_startup_cleanup()
{
    fixture live;
    udp::socket blocker(live.worker.io(), {loopback, static_cast<std::uint16_t>(pool_port + 1U)});
    on_owner(live.worker,
             [&]()
             {
                 require(!live.sender->startup(loopback), "occupied RTCP port did not fail startup");
                 require(!media_port_pool::instance().acquire(), "failed startup lost its pending allocation identity");
                 udp::socket probe(live.worker.io());
                 probe.open(udp::v4());
                 boost::system::error_code error;
                 probe.bind({loopback, pool_port}, error);
                 require(error == boost::asio::error::address_in_use, "test did not exercise a successful first bind");
             });
    live.sender->shutdown();
    live.worker.io().poll();
    blocker.close();
    live.close();

    auto allocation = media_port_pool::instance().acquire();
    require(allocation == pool_port, "pool exhaustion setup failed");
    auto parent = std::make_shared<parent_owner>();
    parent->sender = std::make_shared<gb28181_audio_sender>(live.worker, [parent](boost::system::error_code) { ++parent->errors; });
    const std::weak_ptr<parent_owner> parent_lifetime = parent;
    const std::weak_ptr<gb28181_audio_sender> lifetime = parent->sender;
    on_owner(live.worker,
             [&]()
             {
                 require(!parent->sender->startup(loopback) && !parent->sender->local_port(), "exhausted pool allocated a port");
                 parent->sender->shutdown();
                 parent.reset();
             });
    require(parent_lifetime.expired() && lifetime.expired(), "startup failure without a port retained the parent callback");
    require(!media_port_pool::instance().acquire(), "failed sender returned another owner's allocation");
    media_port_pool::instance().release(*allocation);
    require_port_pair_released(live.worker);
    std::cout << "PCMA partial second-socket bind failure and exhausted-pool callback cleanup: PASS\n";
}

void peer_bye_and_foreign_rtcp()
{
    fixture live;
    on_owner(live.worker,
             [&]()
             {
                 require(live.sender->startup(loopback), "BYE startup failed");
                 live.configure();
             });
    // 对端RTCP身份与发送媒体SSRC不同；BYE无需先收到媒体或SR。
    constexpr std::array<std::uint8_t, 8> bye{0x81, 203, 0, 1, 0x99, 0x88, 0x77, 0x66};
    const udp::endpoint target(loopback, static_cast<std::uint16_t>(pool_port + 1U));
    // 本通道不用XR；零长度未知block不能让第三方通用解析器阻塞owner。
    constexpr std::array<std::uint8_t, 12> unused_xr{0x80, 207, 0, 2, 0x99, 0x88, 0x77, 0x66, 0xff, 0, 0, 0};
    std::cout << "PCMA unused RTCP input guard: checking owner progress\n" << std::flush;
    live.rtcp.send_to(boost::asio::buffer(unused_xr), target);
    live.worker.io().run_for(30ms);
    require(live.errors == 0 && live.sender->local_port() == pool_port, "unused RTCP feedback ended the sender");
    const std::vector<std::vector<std::uint8_t>> ignored{
        {0x80, 203, 0, 0},
        {0x82, 203, 0, 1, 0x99, 0x88, 0x77, 0x66},
        {0x81, 203, 0, 1, 0x99, 0x88, 0x77, 0x66, 0x80},
        {0xa1, 203, 0, 1, 0x99, 0x88, 0x77, 0},
        {0x81, 203, 0, 2, 0x99, 0x88, 0x77, 0x66, 5, 0, 0, 0},
    };
    for (const auto& packet : ignored)
    {
        live.rtcp.send_to(boost::asio::buffer(packet), target);
        live.worker.io().run_for(10ms);
        require(live.errors == 0 && live.sender->local_port() == pool_port, "malformed RTCP was accepted as peer BYE");
    }
    live.rtp.send_to(boost::asio::buffer(bye), target);
    live.worker.io().run_for(30ms);
    require(live.errors == 0 && live.sender->local_port() == pool_port, "foreign RTCP source ended the sender");
    auto compound_bye = std::vector<std::uint8_t>(unused_xr.begin(), unused_xr.end());
    compound_bye.insert(compound_bye.end(), bye.begin(), bye.end());
    live.rtcp.send_to(boost::asio::buffer(compound_bye), target);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (live.sender->local_port() && std::chrono::steady_clock::now() < deadline)
    {
        live.worker.io().run_one_for(1ms);
    }
    require(live.errors == 1 && live.last_error == boost::asio::error::connection_reset, "peer BYE was not reported exactly once");
    require(!live.sender->local_port(), "peer BYE did not close the port pair");
    const std::weak_ptr<gb28181_audio_sender> lifetime = live.sender;
    live.sender->shutdown();
    live.sender.reset();
    live.worker.io().run_for(50ms);
    require(lifetime.expired() && live.errors == 1, "BYE shutdown repeated callbacks or retained the sender");
    require_port_pair_released(live.worker);
    std::cout << "PCMA foreign RTCP ignored and peer BYE error/cleanup: PASS\n";
}

void initialize_pool()
{
    boost::asio::io_context io;
    for (std::uint16_t port = 24520; port < 24720; port = static_cast<std::uint16_t>(port + 2U))
    {
        boost::system::error_code error;
        udp::socket rtp(io);
        rtp.open(udp::v4());
        rtp.bind({loopback, port}, error);
        if (error)
        {
            continue;
        }
        udp::socket rtcp(io);
        rtcp.open(udp::v4());
        rtcp.bind({loopback, static_cast<std::uint16_t>(port + 1U)}, error);
        if (!error)
        {
            pool_port = port;
            media_port_pool::init(port, static_cast<int>(port) + 1);
            return;
        }
    }
    throw std::runtime_error("no free loopback test port pair");
}

void valid_bye_reason_and_padding()
{
    const std::vector<std::vector<std::uint8_t>> packets{
        {0x81, 203, 0, 3, 0x99, 0x88, 0x77, 0x66, 4, 'd', 'o', 'n', 'e', 0, 0, 0},
        {0xa1, 203, 0, 2, 0x99, 0x88, 0x77, 0x66, 0, 0, 0, 4},
    };
    for (const auto& packet : packets)
    {
        fixture live;
        on_owner(live.worker,
                 [&]()
                 {
                     require(live.sender->startup(loopback), "valid-BYE startup failed");
                     live.configure();
                 });
        live.rtcp.send_to(boost::asio::buffer(packet), {loopback, static_cast<std::uint16_t>(pool_port + 1U)});
        live.worker.io().run_for(50ms);
        require(live.errors == 1 && live.last_error == boost::asio::error::connection_reset, "valid BYE reason/padding was rejected");
        require(!live.sender->local_port(), "valid BYE did not release its port pair");
        const std::weak_ptr<gb28181_audio_sender> lifetime = live.sender;
        live.sender.reset();
        live.worker.io().run_for(20ms);
        require(lifetime.expired(), "valid BYE retained the sender");
        require_port_pair_released(live.worker);
    }
    std::cout << "PCMA valid BYE reason/padding and empty-BYE handling: PASS\n";
}

void actual_send_error_and_reentrant_close()
{
    worker_context worker;
    udp::socket rtcp(worker.io(), {loopback, 0});
    auto parent = std::make_shared<parent_owner>();
    unsigned errors{};
    parent->sender =
        std::make_shared<gb28181_audio_sender>(worker,
                                               [parent, &errors](boost::system::error_code error)
                                               {
                                                   require(static_cast<bool>(error), "actual send failure delivered a successful completion");
                                                   ++errors;
                                                   parent->sender->shutdown();
                                                   parent->sender->shutdown();
                                               });
    const std::weak_ptr<parent_owner> parent_lifetime = parent;
    const std::weak_ptr<gb28181_audio_sender> lifetime = parent->sender;
    on_owner(worker,
             [&]()
             {
                 require(parent->sender->startup(loopback), "send-error startup failed");
                 // 仅用地址族不匹配制造真实async_send_to错误，不代表产品接受这种SIP peer。
                 const udp::endpoint incompatible(boost::asio::ip::address_v6::loopback(), 9);
                 require(parent->sender->set_peer(incompatible, rtcp.local_endpoint(), media_ssrc), "send-error peer setup failed");
                 std::array<std::uint8_t, 160> samples{};
                 require(parent->sender->input_audio(samples), "send failure was reported as synchronous queue rejection");
                 require(errors == 0, "send completion unexpectedly ran inside input_audio");
                 parent.reset();
             });
    worker.io().run_for(50ms);
    require(errors == 1, "actual send error was not reported exactly once");
    require(parent_lifetime.expired() && lifetime.expired(), "reentrant error shutdown retained the parent/child callback");
    require_port_pair_released(worker);
    worker.request_stop();
    worker.io().run_for(1s);
    require(worker.io().stopped(), "send-error shutdown did not drain");
    std::cout << "PCMA real async_send_to error and reentrant parent shutdown: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        initialize_pool();
        media_and_sender_report();
        overflow_recovers();
        in_flight_close_breaks_parent_cycle();
        close_before_peer_and_worker_stop();
        partial_startup_cleanup();
        peer_bye_and_foreign_rtcp();
        valid_bye_reason_and_padding();
        actual_send_error_and_reentrant_close();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "GB audio sender: " << error.what() << '\n';
        return 1;
    }
}
