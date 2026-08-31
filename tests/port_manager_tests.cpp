#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_udp_session.h"
#include "media/gb28181/gb28181_udp_output_session.h"
#include "media/net/port_manager.h"

namespace media_server
{
namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void test_single_reservation()
{
    port_manager::init(100, 103);
    auto& manager = port_manager::instance();
    const auto first = manager.acquire();
    const auto second = manager.acquire();
    require(first && second && *first != *second, "single reservations are unique");
    manager.release(*first);
    const auto reused = manager.acquire();
    require(reused && *reused == *first, "released single port is reusable");
    manager.release(*second);
    manager.release(*reused);
    manager.release(*reused);
    port_manager::destroy();
}

void test_pair_reservation()
{
    port_manager::init(200, 205);
    auto& manager = port_manager::instance();
    const auto pair = manager.acquire_pair();
    require(pair && (pair->first % 2U) == 0U && pair->second == pair->first + 1U, "pair is adjacent even odd");
    const auto single = manager.acquire();
    require(single && *single != pair->first && *single != pair->second, "pair ports are reserved atomically");
    manager.release(*single);
    manager.release(*pair);
    const auto reused = manager.acquire_pair();
    require(reused && reused->first == pair->first && reused->second == pair->second, "released pair is reusable");
    manager.release(*reused);
    port_manager::destroy();
}

void test_exhaustion()
{
    port_manager::init(300, 301);
    auto& manager = port_manager::instance();
    require(manager.acquire() && manager.acquire(), "single range fills");
    require(!manager.acquire(), "single range exhaustion");
    port_manager::destroy();

    port_manager::init(302, 303);
    auto& pair_manager = port_manager::instance();
    require(pair_manager.acquire_pair().has_value(), "pair range fills");
    require(!pair_manager.acquire_pair(), "pair range exhaustion");
    port_manager::destroy();
}

void test_concurrent_reservation()
{
    port_manager::init(400, 799);
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t reservations_per_thread = 32;
    std::mutex mutex;
    std::vector<std::uint16_t> reservations;
    reservations.reserve(thread_count * reservations_per_thread);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index)
    {
        threads.emplace_back(
            [&]
            {
                std::vector<std::uint16_t> local;
                local.reserve(reservations_per_thread);
                for (std::size_t count = 0; count < reservations_per_thread; ++count)
                {
                    const auto port = port_manager::instance().acquire();
                    require(port.has_value(), "concurrent reservation available");
                    local.push_back(*port);
                }
                std::scoped_lock lock(mutex);
                reservations.insert(reservations.end(), local.begin(), local.end());
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    const std::set<std::uint16_t> unique(reservations.begin(), reservations.end());
    require(unique.size() == reservations.size(), "concurrent reservations are unique");
    for (const auto port : reservations)
    {
        port_manager::instance().release(port);
    }
    port_manager::destroy();
}

media_track make_video_track()
{
    return media_track{.id = 1,
                       .kind = media_kind::video,
                       .codec = codec_id::h264,
                       .clock_rate = 90'000,
                       .channel_count = 0,
                       .codec_config = {},
                       .config_version = 0};
}

gb28181_description make_udp_description()
{
    return gb28181_description{.transport = gb28181_transport::udp,
                               .address = boost::asio::ip::address_v4::loopback(),
                               .rtp_port = 50'000,
                               .rtcp_port = 50'001,
                               .payload_type = 96,
                               .ssrc = 10'000'2001};
}

void test_udp_output_releases_pair_after_shutdown()
{
    boost::asio::io_context io;
    port_manager::init(32'400, 32'401);
    auto stream = std::make_shared<media_stream>("live/port-release", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "port release stream tracks");
    require(registry::instance().add(stream), "port release stream registry");
    auto session = std::make_shared<gb28181_udp_output_session>(
        io.get_executor(), stream, make_udp_description(), boost::asio::ip::address_v4::loopback(), "output", false);
    require(registry::instance().add_output_session(stream->name(), "output", session), "port release output registry");
    require(session->startup(), "port release output startup");

    boost::system::error_code bind_error;
    boost::asio::ip::udp::socket other_address(io);
    other_address.open(boost::asio::ip::udp::v4());
    other_address.bind({boost::asio::ip::make_address_v4("127.0.0.2"), 32'400}, bind_error);
    require(!bind_error, "udp output only binds configured local address");

    session->shutdown();
    io.run();
    session.reset();

    const auto pair = port_manager::instance().acquire_pair();
    require(pair && pair->first == 32'400 && pair->second == 32'401, "port release after shutdown");
    port_manager::instance().release(*pair);
    registry::instance().clear();
    port_manager::destroy();
}

void test_udp_output_releases_pair_after_bind_failure()
{
    boost::asio::io_context io;
    port_manager::init(32'410, 32'411);
    boost::asio::ip::udp::socket occupied(io, {boost::asio::ip::address_v4::loopback(), 32'410});
    auto stream = std::make_shared<media_stream>("live/port-bind-failure", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "port bind failure stream tracks");
    require(registry::instance().add(stream), "port bind failure stream registry");
    auto session = std::make_shared<gb28181_udp_output_session>(
        io.get_executor(), stream, make_udp_description(), boost::asio::ip::address_v4::loopback(), "output", false);
    require(!session->startup(), "port bind failure output startup");

    const auto pair = port_manager::instance().acquire_pair();
    require(pair && pair->first == 32'410 && pair->second == 32'411, "port release after bind failure");
    port_manager::instance().release(*pair);
    registry::instance().clear();
    port_manager::destroy();
}

void test_udp_input_releases_pair_after_bind_failure()
{
    boost::asio::io_context io;
    port_manager::init(32'420, 32'421);
    boost::asio::ip::udp::socket occupied(io, {boost::asio::ip::address_v4::loopback(), 32'420});
    const gb28181_description description{.transport = gb28181_transport::udp,
                                          .address = boost::asio::ip::address_v4::loopback(),
                                          .payload_type = 96,
                                          .ssrc = 10'000'2001};
    auto session = std::make_shared<gb28181_udp_session>(io.get_executor(), "live/input-port-bind-failure", description);
    require(!session->startup(), "input port bind failure startup");

    const auto pair = port_manager::instance().acquire_pair();
    require(pair && pair->first == 32'420 && pair->second == 32'421, "input port release after bind failure");
    port_manager::instance().release(*pair);
    port_manager::destroy();
}

void test_udp_input_rejects_unavailable_local_address()
{
    boost::asio::io_context io;
    port_manager::init(32'430, 32'431);
    const gb28181_description description{.transport = gb28181_transport::udp,
                                          .address = boost::asio::ip::make_address("192.0.2.1"),
                                          .payload_type = 96,
                                          .ssrc = 10'000'2001};
    auto session = std::make_shared<gb28181_udp_session>(io.get_executor(), "live/input-unavailable-address", description);
    require(!session->startup(), "input unavailable local address rejected");

    const auto pair = port_manager::instance().acquire_pair();
    require(pair && pair->first == 32'430 && pair->second == 32'431, "input unavailable address releases pair");
    port_manager::instance().release(*pair);
    port_manager::destroy();
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_single_reservation();
        media_server::test_pair_reservation();
        media_server::test_exhaustion();
        media_server::test_concurrent_reservation();
        media_server::registry::init();
        media_server::test_udp_output_releases_pair_after_shutdown();
        media_server::test_udp_output_releases_pair_after_bind_failure();
        media_server::test_udp_input_releases_pair_after_bind_failure();
        media_server::test_udp_input_rejects_unavailable_local_address();
        media_server::registry::destroy();
        std::cout << "[pass] port_manager_tests\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] port_manager_tests: " << error.what() << '\n';
        return 1;
    }
}
