#include <string>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_service.h"

namespace media_server
{
namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string{message});
    }
}

std::string make_tcp_active_sdp(std::uint16_t port, std::uint32_t ssrc)
{
    return "v=0\r\n"
           "o=34020000002000000001 0 0 IN IP4 127.0.0.1\r\n"
           "s=Play\r\n"
           "c=IN IP4 127.0.0.1\r\n"
           "t=0 0\r\n"
           "m=video " +
           std::to_string(port) +
           " TCP/RTP/AVP 96\r\n"
           "a=rtpmap:96 PS/90000\r\n"
           "a=recvonly\r\n"
           "a=setup:active\r\n"
           "a=connection:new\r\n"
           "y=" +
           std::to_string(ssrc) + "\r\n";
}

media_track make_video_track()
{
    return media_track{
        .id = 1,
        .kind = media_kind::video,
        .codec = codec_id::h264,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = {},
        .config_version = 0,
    };
}

std::shared_ptr<media_stream> add_video_stream(stream_registry& registry, boost::asio::io_context& io, std::string name)
{
    auto stream = std::make_shared<media_stream>(std::move(name), io.get_executor());
    require(stream->set_tracks({make_video_track()}), "gb output tracks");
    require(registry.add(stream), "gb output registry");
    return stream;
}

void test_input_identity_is_reusable_immediately_after_remove()
{
    boost::asio::io_context io;
    stream_registry registry;
    gb28181_service service(registry);
    const auto sdp = make_tcp_active_sdp(65'000, 10'000'2001);

    require(service.create(io.get_executor(), "live/gb-identity", sdp, std::nullopt, std::nullopt) == gb28181_create_error::none,
            "gb input first create");
    require(service.remove("live/gb-identity"), "gb input remove");
    require(service.create(io.get_executor(), "live/gb-identity", sdp, std::nullopt, std::nullopt) == gb28181_create_error::none,
            "gb input identity reusable immediately after remove");

    service.shutdown();
    io.run();
}

void test_output_identity_is_reusable_immediately_after_remove()
{
    boost::asio::io_context io;
    stream_registry registry;
    const auto stream = add_video_stream(registry, io, "live/gb-output-identity");
    gb28181_service service(registry);
    const auto sdp = make_tcp_active_sdp(65'000, 10'000'2002);

    require(service.create_output(io.get_executor(), stream->name(), "primary", false, sdp) == gb28181_output_create_error::none,
            "gb output first create");
    require(service.remove_output(stream->name(), "primary"), "gb output remove");
    require(service.create_output(io.get_executor(), stream->name(), "primary", false, sdp) == gb28181_output_create_error::none,
            "gb output identity reusable immediately after remove");

    service.shutdown();
    io.run();
}

void test_input_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    stream_registry registry;
    gb28181_service service(registry);
    const auto sdp = make_tcp_active_sdp(peer.local_endpoint().port(), 10'000'2003);

    require(service.create(io.get_executor(), "live/gb-generation", sdp, std::nullopt, std::nullopt) == gb28181_create_error::none,
            "gb input old generation create");
    require(service.remove("live/gb-generation"), "gb input old generation remove");
    require(service.create(io.get_executor(), "live/gb-generation", sdp, std::nullopt, std::nullopt) == gb28181_create_error::none,
            "gb input replacement create");

    io.poll();
    require(service.create(io.get_executor(), "live/gb-generation", sdp, std::nullopt, std::nullopt) == gb28181_create_error::duplicate_stream,
            "gb input old async work preserves replacement");

    service.shutdown();
    io.run();
}

void test_output_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    stream_registry registry;
    const auto stream = add_video_stream(registry, io, "live/gb-output-generation");
    gb28181_service service(registry);
    const auto sdp = make_tcp_active_sdp(peer.local_endpoint().port(), 10'000'2004);

    require(service.create_output(io.get_executor(), stream->name(), "primary", false, sdp) == gb28181_output_create_error::none,
            "gb output old generation create");
    require(service.remove_output(stream->name(), "primary"), "gb output old generation remove");
    require(service.create_output(io.get_executor(), stream->name(), "primary", false, sdp) == gb28181_output_create_error::none,
            "gb output replacement create");

    io.poll();
    require(service.create_output(io.get_executor(), stream->name(), "primary", false, sdp) == gb28181_output_create_error::duplicate_output,
            "gb output old async work preserves replacement");

    service.shutdown();
    io.run();
}

}    // namespace
}    // namespace media_server

int main()
{
    int failures = 0;
    const auto run = [&failures](std::string_view name, auto&& test)
    {
        try
        {
            test();
            std::cout << "[pass] " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    };

    run("input_identity_is_reusable_immediately_after_remove", media_server::test_input_identity_is_reusable_immediately_after_remove);
    run("output_identity_is_reusable_immediately_after_remove", media_server::test_output_identity_is_reusable_immediately_after_remove);
    run("input_old_async_work_does_not_remove_replacement", media_server::test_input_old_async_work_does_not_remove_replacement);
    run("output_old_async_work_does_not_remove_replacement", media_server::test_output_old_async_work_does_not_remove_replacement);
    return failures == 0 ? 0 : 1;
}
