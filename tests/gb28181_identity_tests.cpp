#include <string>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181.h"

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

gb28181_description make_tcp_active_description(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_description{.transport = gb28181_transport::tcp_active,
                               .address = boost::asio::ip::address_v4::loopback(),
                               .rtp_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
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

std::shared_ptr<media_stream> add_video_stream(boost::asio::io_context& io, std::string name)
{
    auto stream = std::make_shared<media_stream>(std::move(name), io.get_executor());
    require(stream->set_tracks({make_video_track()}), "gb output tracks");
    require(registry::instance().add(stream), "gb output registry");
    return stream;
}

void test_input_identity_is_reusable_immediately_after_remove()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();
    gb28181::shutdown();
    const auto description = make_tcp_active_description(65'000, 10'000'2001);

    require(gb28181::create(io, "live/gb-identity", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::none,
            "gb input first create");
    require(gb28181::remove("live/gb-identity"), "gb input remove");
    require(gb28181::create(io, "live/gb-identity", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::none,
            "gb input identity reusable immediately after remove");

    gb28181::shutdown();
    io.run();
    io.restart();
    require(gb28181::create(io, "live/gb-identity", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::none,
            "gb input module reusable after shutdown");
    gb28181::shutdown();
    io.run();
}

void test_output_identity_is_reusable_immediately_after_remove()
{
    boost::asio::io_context io;
    auto& streams = media_server::registry::instance();
    streams.clear();
    const auto stream = add_video_stream(io, "live/gb-output-identity");
    gb28181::shutdown();
    const auto description = make_tcp_active_description(65'000, 10'000'2002);

    require(gb28181::create_output(io, stream->name(), "primary", false, description) == gb28181::gb28181_output_create_error::none,
            "gb output first create");
    require(gb28181::remove_output(stream->name(), "primary"), "gb output remove");
    require(gb28181::create_output(io, stream->name(), "primary", false, description) == gb28181::gb28181_output_create_error::none,
            "gb output identity reusable immediately after remove");

    gb28181::shutdown();
    io.run();
}

void test_input_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    auto& streams = media_server::registry::instance();
    streams.clear();
    gb28181::shutdown();
    const auto description = make_tcp_active_description(peer.local_endpoint().port(), 10'000'2003);

    require(gb28181::create(io, "live/gb-generation", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::none,
            "gb input old generation create");
    require(gb28181::remove("live/gb-generation"), "gb input old generation remove");
    require(gb28181::create(io, "live/gb-generation", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::none,
            "gb input replacement create");

    io.poll();
    require(gb28181::create(io, "live/gb-generation", description, std::nullopt, std::nullopt) == gb28181::gb28181_create_error::duplicate_stream,
            "gb input old async work preserves replacement");

    gb28181::shutdown();
    io.run();
}

void test_output_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    auto& streams = media_server::registry::instance();
    streams.clear();
    const auto stream = add_video_stream(io, "live/gb-output-generation");
    gb28181::shutdown();
    const auto description = make_tcp_active_description(peer.local_endpoint().port(), 10'000'2004);

    require(gb28181::create_output(io, stream->name(), "primary", false, description) == gb28181::gb28181_output_create_error::none,
            "gb output old generation create");
    require(gb28181::remove_output(stream->name(), "primary"), "gb output old generation remove");
    require(gb28181::create_output(io, stream->name(), "primary", false, description) == gb28181::gb28181_output_create_error::none,
            "gb output replacement create");

    io.poll();
    require(gb28181::create_output(io, stream->name(), "primary", false, description) == gb28181::gb28181_output_create_error::duplicate_output,
            "gb output old async work preserves replacement");

    gb28181::shutdown();
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
