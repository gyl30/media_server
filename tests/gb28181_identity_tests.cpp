#include <chrono>
#include <string>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_session_registry.h"
#include "media/gb28181/gb28181_tcp_output_session.h"
#include "media/gb28181/gb28181_tcp_session.h"
#include "media/http/gb28181_http.h"
#include "media/net/tcp_acceptor.h"

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

gb28181_http_request request()
{
    return gb28181_http_request{boost::beast::http::verb::post, "/gb28181/create", 11};
}

void require_status(const gb28181_http_response& response, boost::beast::http::status status, std::string_view message)
{
    require(response.result() == status, message);
}

gb28181_description make_tcp_active_description(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_description{.transport = gb28181_transport::tcp_active,
                               .address = boost::asio::ip::address_v4::loopback(),
                               .rtp_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
}

gb28181_input_config make_input_config(std::string stream_name, gb28181_description description)
{
    return gb28181_input_config{.stream_name = std::move(stream_name),
                                .description = std::move(description),
                                .remote_rtp_endpoint = std::nullopt,
                                .remote_rtcp_port = std::nullopt};
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

gb28181_output_config make_output_config(const std::shared_ptr<media_stream>& stream,
                                         std::string output_id,
                                         gb28181_description description)
{
    return gb28181_output_config{.stream_name = stream->name(),
                                 .output_id = std::move(output_id),
                                 .description = std::move(description),
                                 .rtcp = false};
}

void clear_state()
{
    gb28181_session_registry::instance().clear();
    registry::instance().clear();
}

void test_input_identity_is_reusable_after_remove()
{
    boost::asio::io_context io;
    clear_state();
    const auto description = make_tcp_active_description(65'000, 10'000'2001);

    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-identity", description)),
                   boost::beast::http::status::created,
                   "gb input first create");
    require_status(handle_gb28181_input_delete(request(), "live/gb-identity"), boost::beast::http::status::ok, "gb input remove");
    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-identity", description)),
                   boost::beast::http::status::created,
                   "gb input identity reusable immediately after remove");

    require_status(handle_gb28181_input_delete(request(), "live/gb-identity"), boost::beast::http::status::ok, "gb input second remove");
    io.run();
    io.restart();
    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-identity", description)),
                   boost::beast::http::status::created,
                   "gb input reusable after remove");
    require_status(handle_gb28181_input_delete(request(), "live/gb-identity"), boost::beast::http::status::ok, "gb input final remove");
    io.run();
    clear_state();
}

void test_output_identity_is_reusable_after_remove()
{
    boost::asio::io_context io;
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-identity");
    const auto description = make_tcp_active_description(65'000, 10'000'2002);

    require_status(handle_gb28181_output_create(request(), io, make_output_config(stream, "primary", description)),
                   boost::beast::http::status::created,
                   "gb output first create");
    require_status(handle_gb28181_output_delete(request(), stream->name(), "primary"), boost::beast::http::status::ok, "gb output remove");
    require_status(handle_gb28181_output_create(request(), io, make_output_config(stream, "primary", description)),
                   boost::beast::http::status::created,
                   "gb output identity reusable immediately after remove");

    require_status(handle_gb28181_output_delete(request(), stream->name(), "primary"), boost::beast::http::status::ok, "gb output second remove");
    io.run();
    clear_state();
}

void test_input_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    clear_state();
    const auto description = make_tcp_active_description(peer.local_endpoint().port(), 10'000'2003);

    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-generation", description)),
                   boost::beast::http::status::created,
                   "gb input old generation create");
    require_status(handle_gb28181_input_delete(request(), "live/gb-generation"), boost::beast::http::status::ok, "gb input old generation remove");
    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-generation", description)),
                   boost::beast::http::status::created,
                   "gb input replacement create");

    io.poll();
    require_status(handle_gb28181_input_create(request(), io, make_input_config("live/gb-generation", description)),
                   boost::beast::http::status::internal_server_error,
                   "gb input old async work preserves replacement");

    require_status(handle_gb28181_input_delete(request(), "live/gb-generation"), boost::beast::http::status::ok, "gb input replacement remove");
    io.run();
    clear_state();
}

void test_output_old_async_work_does_not_remove_replacement()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor peer(io, {boost::asio::ip::tcp::v4(), 0});
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-generation");
    const auto description = make_tcp_active_description(peer.local_endpoint().port(), 10'000'2004);

    require_status(handle_gb28181_output_create(request(), io, make_output_config(stream, "primary", description)),
                   boost::beast::http::status::created,
                   "gb output old generation create");
    require_status(handle_gb28181_output_delete(request(), stream->name(), "primary"), boost::beast::http::status::ok, "gb output old generation remove");
    require_status(handle_gb28181_output_create(request(), io, make_output_config(stream, "primary", description)),
                   boost::beast::http::status::created,
                   "gb output replacement create");

    io.poll();
    require_status(handle_gb28181_output_create(request(), io, make_output_config(stream, "primary", description)),
                   boost::beast::http::status::internal_server_error,
                   "gb output old async work preserves replacement");

    require_status(handle_gb28181_output_delete(request(), stream->name(), "primary"), boost::beast::http::status::ok, "gb output replacement remove");
    io.run();
    clear_state();
}

void test_tcp_timeout_unregisters_input_session()
{
    boost::asio::io_context io;
    clear_state();
    const std::string stream_name = "live/gb-input-timeout";
    auto source = std::make_shared<tcp_acceptor>(io.get_executor(),
                                                 0,
                                                 boost::asio::ip::address_v4::loopback(),
                                                 std::chrono::milliseconds(5));
    auto session = std::make_shared<gb28181_tcp_session>(io.get_executor(), source, stream_name, 96, 10'000'2005);
    require(gb28181_session_registry::instance().add_input(stream_name, session), "gb input timeout registry add");
    require(session->startup(), "gb input timeout startup");
    io.run();

    const auto remaining = gb28181_session_registry::instance().take_input(stream_name);
    require(!remaining, "gb input timeout unregisters session");
    clear_state();
}

void test_tcp_timeout_unregisters_output_session()
{
    boost::asio::io_context io;
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-timeout");
    auto source = std::make_shared<tcp_acceptor>(io.get_executor(),
                                                 0,
                                                 boost::asio::ip::address_v4::loopback(),
                                                 std::chrono::milliseconds(5));
    auto session = std::make_shared<gb28181_tcp_output_session>(io.get_executor(),
                                                                source,
                                                                std::weak_ptr<media_stream>{stream},
                                                                stream->name(),
                                                                "timeout",
                                                                96,
                                                                10'000'2006);
    require(gb28181_session_registry::instance().add_output(stream->name(), "timeout", session),
            "gb output timeout registry add");
    require(session->startup(), "gb output timeout startup");
    io.run();

    const auto remaining = gb28181_session_registry::instance().take_output(stream->name(), "timeout");
    require(!remaining, "gb output timeout unregisters session");
    clear_state();
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

    run("input_identity_is_reusable_after_remove", media_server::test_input_identity_is_reusable_after_remove);
    run("output_identity_is_reusable_after_remove", media_server::test_output_identity_is_reusable_after_remove);
    run("input_old_async_work_does_not_remove_replacement", media_server::test_input_old_async_work_does_not_remove_replacement);
    run("output_old_async_work_does_not_remove_replacement", media_server::test_output_old_async_work_does_not_remove_replacement);
    run("tcp_timeout_unregisters_input_session", media_server::test_tcp_timeout_unregisters_input_session);
    run("tcp_timeout_unregisters_output_session", media_server::test_tcp_timeout_unregisters_output_session);
    return failures == 0 ? 0 : 1;
}
