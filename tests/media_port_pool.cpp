#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/post.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"
#include "media/gb28181/gb28181_udp_sender_session.h"
#include "media/rtsp/rtsp_publish_session.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace
{
using namespace media_server;
using udp = boost::asio::ip::udp;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void allocation(int start, int end, const std::set<std::uint16_t>& expected)
{
    media_port_pool::init(start, end);
    auto& pool = media_port_pool::instance();
    std::set<std::uint16_t> allocated;
    while (const auto port = pool.acquire())
    {
        require(*port % 2 == 0 && static_cast<int>(*port) + 1 <= end, "incomplete or odd allocation");
        require(allocated.insert(*port).second, "duplicate allocation");
    }
    require(allocated == expected, "allocation range differs");
    const auto port = *allocated.begin();
    pool.release(port);
    pool.release(port);
    pool.release(static_cast<std::uint16_t>(port + 1U));
    pool.release(0);
    pool.release(65'535);
    require(pool.acquire() == port && !pool.acquire(), "invalid release duplicated an available port");
    for (const auto value : allocated)
    {
        pool.release(value);
    }

    std::mutex held_mutex;
    std::set<std::uint16_t> held;
    std::atomic_bool duplicate{};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index)
    {
        threads.emplace_back(
            [&]()
            {
                for (int iteration = 0; iteration < 1'000; ++iteration)
                {
                    std::optional<std::uint16_t> value;
                    while (!(value = pool.acquire()))
                    {
                        std::this_thread::yield();
                    }
                    {
                        std::scoped_lock lock(held_mutex);
                        if (!held.insert(*value).second)
                        {
                            duplicate = true;
                        }
                    }
                    std::this_thread::yield();
                    {
                        std::scoped_lock lock(held_mutex);
                        held.erase(*value);
                    }
                    pool.release(*value);
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    require(!duplicate && held.empty(), "concurrent owners shared an allocation");
    allocated.clear();
    while (const auto value = pool.acquire())
    {
        require(allocated.insert(*value).second, "concurrent release duplicated allocation");
    }
    require(allocated == expected, "concurrent acquire/release leaked allocation");
}

void bind_failure()
{
    media_port_pool::init(50'000, 50'003);
    auto& pool = media_port_pool::instance();
    const auto address = boost::asio::ip::make_address("127.0.0.1");
    worker_context worker;
    std::exception_ptr failure;
    for (const std::string_view protocol : {"receiver", "sender", "rtsp"})
    {
        for (const auto blocked_port : std::array<std::uint16_t, 2>{50'000, 50'001})
        {
            udp::socket blocker(worker.io(), udp::endpoint(address, blocked_port));
            if (protocol == "sender")
            {
                auto stream = std::make_shared<media_stream>("verify/port-conflict", worker);
                require(stream->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                             .clock_rate = 90'000, .channel_count = 0, .codec_config = {}}}), "test tracks rejected");
                auto session = std::make_shared<gb28181_udp_sender_session>(
                    worker, stream, "sender", udp::endpoint(address, 40'000), std::nullopt);
                require(!session->startup(address, 96, 1234), "sender retried another allocation after bind failure");
                session->shutdown();
            }
            else if (protocol == "receiver")
            {
                auto session = std::make_shared<gb28181_udp_receiver_session>(worker, "verify/port-conflict", 96, 1234);
                require(!session->startup(address), "receiver retried another allocation after bind failure");
                session->shutdown();
            }
            else
            {
                rtsp_handler_t handler{};
                handler.close = [](void*) { return 0; };
                handler.send = [](void*, const void*, std::size_t) { return 0; };
                std::unique_ptr<rtsp_server_t, decltype(&rtsp_server_destroy)> server(
                    rtsp_server_create("127.0.0.1", 8554, &handler, nullptr, nullptr), &rtsp_server_destroy);
                require(server != nullptr, "RTSP test server creation failed");
                auto session = std::make_shared<rtsp_publish_session>(
                    worker, "verify/port-conflict", address, [](std::span<const std::uint8_t>) {}, []() {});
                constexpr std::string_view sdp =
                    "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=port-test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                    "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                    "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==\r\n"
                    "a=control:trackID=1\r\n";
                require(session->on_announce(server.get(), "rtsp://127.0.0.1/verify/port-conflict",
                                             sdp.data(), static_cast<int>(sdp.size())), "RTSP test ANNOUNCE failed");
                rtsp_header_transport_t transport{};
                require(rtsp_header_transport("RTP/AVP;unicast;client_port=40000-40001;mode=record", &transport) == 0,
                        "RTSP test transport rejected");
                require(session->on_setup(server.get(), "rtsp://127.0.0.1/verify/port-conflict/trackID=1", {}, &transport, 1) < 0,
                        "RTSP retried another allocation after bind failure");
                session->shutdown();
            }
            // 失败的 startup 不自行清理，端口由 shutdown 投递的 safe_shutdown 归还。
            worker.io().poll();
            const auto first = pool.acquire();
            const auto second = pool.acquire();
            require(first == 50'000 && second == 50'002 && !pool.acquire(), "bind failure leaked or duplicated allocation");
            if (blocked_port == 50'001)
            {
                udp::socket released_rtp(worker.io(), udp::endpoint(address, *first));
            }
            blocker.close();
            {
                udp::socket rtp(worker.io(), udp::endpoint(address, *first));
                udp::socket rtcp(worker.io(), udp::endpoint(address, static_cast<std::uint16_t>(*first + 1U)));
                udp::socket unused(worker.io(), udp::endpoint(address, *second));
            }
            pool.release(*second);
            pool.release(*first);
        }
    }

    boost::asio::post(
        worker.io(),
        [&]()
        {
            try
            {
                auto receiver = std::make_shared<gb28181_udp_receiver_session>(worker, "verify/port-reuse", 96, 1234);
                require(receiver->startup(address) == 50'000, "receiver did not reuse released allocation");
                receiver->shutdown();
                boost::asio::post(
                    worker.io(),
                    [&, receiver]()
                    {
                        try
                        {
                            const auto port = pool.acquire();
                            require(port == 50'000, "shutdown did not release allocation");
                            {
                                udp::socket rtp(worker.io(), udp::endpoint(address, *port));
                                udp::socket rtcp(worker.io(), udp::endpoint(address, static_cast<std::uint16_t>(*port + 1U)));
                            }
                            pool.release(*port);
                        }
                        catch (...)
                        {
                            failure = std::current_exception();
                        }
                        worker.request_stop();
                    });
            }
            catch (...)
            {
                failure = std::current_exception();
                worker.request_stop();
            }
        });
    worker.io().run();
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}
}    // namespace

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "expected test case");
        const std::string_view mode{argv[1]};
        if (mode == "bind_failure")
        {
            bind_failure();
        }
        else if (mode == "odd")
        {
            allocation(10'001, 10'006, {10'002, 10'004});
        }
        else if (mode == "maximum")
        {
            allocation(65'533, 65'535, {65'534});
        }
        else
        {
            require(mode == "allocation", "unknown test case");
            for (const auto range : {std::array{0, 10}, std::array{-1, 10}, std::array{10, 9},
                                     std::array{10, 65'536}, std::array{10'000, 10'000},
                                     std::array{10'001, 10'002}, std::array{65'535, 65'535}})
            {
                bool rejected{};
                try
                {
                    media_port_pool::init(range[0], range[1]);
                }
                catch (const std::invalid_argument&)
                {
                    rejected = true;
                }
                require(rejected, "invalid range accepted");
            }
            allocation(10'000, 10'007, {10'000, 10'002, 10'004, 10'006});
        }
        std::cout << "media port pool " << mode << ": PASS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
