#include <chrono>
#include <string>
#include <cstdint>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/json.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/io_context.hpp>

#include "media/net/worker_context.h"
#include "media/core/media_stream.h"
#include "media/http/gb28181_http.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"

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

void require_status(const gb28181_http_response& response, boost::beast::http::status status, std::string_view message)
{
    require(response.result() == status, message);
}

gb28181_http_request request(std::string target, boost::json::object body)
{
    gb28181_http_request value{boost::beast::http::verb::post, std::move(target), 11};
    value.set(boost::beast::http::field::content_type, "application/json");
    value.body() = boost::json::serialize(body);
    value.prepare_payload();
    return value;
}

gb28181_http_response receiver_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "gb receiver target");
    return handle_gb28181_receiver_request(request, worker, *target, boost::asio::ip::address_v4::loopback());
}

gb28181_http_response sender_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "gb sender target");
    return handle_gb28181_sender_request(request, worker, *target, boost::asio::ip::address_v4::loopback());
}

gb28181_transport_config make_tcp_active_transport(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_transport_config{.mode = gb28181_transport::tcp_active,
                               .remote_address = boost::asio::ip::address_v4::loopback(),
                               .remote_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
}

gb28181_transport_config make_tcp_passive_transport(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_transport_config{.mode = gb28181_transport::tcp_passive,
                               .listen_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
}

gb28181_http_response create_receiver(worker_context& worker, std::string_view stream_name, const gb28181_transport_config& transport)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    body["transport"] = "tcp_active";
    body["remote_address"] = transport.remote_address.to_string();
    body["remote_port"] = transport.remote_port;
    body["payload_type"] = transport.payload_type;
    body["ssrc"] = transport.ssrc;
    return receiver_request(worker, request("/gb28181/receiver/create", std::move(body)));
}

gb28181_http_response delete_receiver(worker_context& worker, std::string_view stream_name)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    return receiver_request(worker, request("/gb28181/receiver/delete", std::move(body)));
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

std::shared_ptr<media_stream> add_video_stream(boost::asio::io_context& io, std::string name)
{
    auto stream = std::make_shared<media_stream>(std::move(name), io.get_executor());
    require(stream->set_tracks({make_video_track()}), "gb sender tracks");
    require(registry::instance().add(stream), "gb sender registry");
    return stream;
}

gb28181_http_response create_sender(worker_context& worker,
                                    const media_stream& stream,
                                    std::string_view sender_id,
                                    const gb28181_transport_config& transport)
{
    boost::json::object body;
    body["stream_name"] = stream.name();
    body["sender_id"] = sender_id;
    body["transport"] = "tcp_active";
    body["remote_address"] = transport.remote_address.to_string();
    body["remote_port"] = transport.remote_port;
    body["payload_type"] = transport.payload_type;
    body["ssrc"] = transport.ssrc;
    return sender_request(worker, request("/gb28181/sender/create", std::move(body)));
}

gb28181_http_response delete_sender(worker_context& worker, std::string_view stream_name, std::string_view sender_id)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    body["sender_id"] = sender_id;
    return sender_request(worker, request("/gb28181/sender/delete", std::move(body)));
}

void clear_state() { registry::instance().clear(); }

void test_receiver_identity_is_reusable_after_shutdown()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto description = make_tcp_active_transport(65'000, 10'000'2001);

    require_status(create_receiver(worker, "live/gb-identity", description), boost::beast::http::status::created, "gb receiver first create");
    require_status(delete_receiver(worker, "live/gb-identity"), boost::beast::http::status::ok, "gb receiver remove");
    require_status(create_receiver(worker, "live/gb-identity", description), boost::beast::http::status::created, "gb receiver reusable after shutdown");
    require_status(delete_receiver(worker, "live/gb-identity"), boost::beast::http::status::ok, "gb receiver final remove");
    io.run();
    clear_state();
}

void test_sender_identity_is_reusable_after_shutdown()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-sender-identity");
    const auto description = make_tcp_active_transport(65'000, 10'000'2002);

    require_status(create_sender(worker, *stream, "primary", description), boost::beast::http::status::created, "gb sender first create");
    require_status(delete_sender(worker, stream->name(), "primary"), boost::beast::http::status::ok, "gb sender remove");
    require_status(create_sender(worker, *stream, "primary", description), boost::beast::http::status::created, "gb sender reusable after shutdown");
    require_status(delete_sender(worker, stream->name(), "primary"), boost::beast::http::status::ok, "gb sender final remove");
    io.run();
    clear_state();
}

void test_tcp_receiver_repeated_shutdown_is_idempotent()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const std::string stream_name = "live/gb-receiver-repeated-shutdown";
    const auto description = make_tcp_passive_transport(0, 10'000'2007);
    auto session = std::make_shared<gb28181_tcp_receiver_session>(worker, stream_name, description, boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    require(registry::instance().add_receiver_session(stream_name, session), "gb receiver repeated shutdown registry add");
    require(session->startup(), "gb receiver repeated shutdown startup");

    session->shutdown();
    session->shutdown();
    session->shutdown();
    io.run();

    const auto remaining = registry::instance().take_receiver_session(stream_name);
    require(!remaining, "gb receiver repeated shutdown unregisters session");
    clear_state();
}

void test_tcp_sender_repeated_shutdown_is_idempotent()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-sender-repeated-shutdown");
    const auto description = make_tcp_passive_transport(0, 10'000'2008);
    auto session = std::make_shared<gb28181_tcp_sender_session>(worker,
                                                                std::weak_ptr<media_stream>{stream},
                                                                stream->name(),
                                                                "repeated-shutdown",
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                std::chrono::seconds(1));
    require(registry::instance().add_sender_session(stream->name(), "repeated-shutdown", session), "gb sender repeated shutdown registry add");
    require(session->startup(), "gb sender repeated shutdown startup");

    session->shutdown();
    session->shutdown();
    session->shutdown();
    io.run();

    const auto remaining = registry::instance().take_sender_session(stream->name(), "repeated-shutdown");
    require(!remaining, "gb sender repeated shutdown unregisters session");
    clear_state();
}

void test_tcp_timeout_unregisters_receiver_session()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const std::string stream_name = "live/gb-receiver-timeout";
    const auto description = make_tcp_passive_transport(0, 10'000'2005);
    auto session = std::make_shared<gb28181_tcp_receiver_session>(worker, stream_name, description, boost::asio::ip::address_v4::loopback(), std::chrono::milliseconds(5));
    require(registry::instance().add_receiver_session(stream_name, session), "gb receiver timeout registry add");
    require(session->startup(), "gb receiver timeout startup");
    io.run();

    const auto remaining = registry::instance().take_receiver_session(stream_name);
    require(!remaining, "gb receiver timeout unregisters session");
    clear_state();
}

void test_tcp_timeout_unregisters_sender_session()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-sender-timeout");
    const auto description = make_tcp_passive_transport(0, 10'000'2006);
    auto session = std::make_shared<gb28181_tcp_sender_session>(worker,
                                                                std::weak_ptr<media_stream>{stream},
                                                                stream->name(),
                                                                "timeout",
                                                                description,
                                                                boost::asio::ip::address_v4::loopback(),
                                                                std::chrono::milliseconds(5));
    require(registry::instance().add_sender_session(stream->name(), "timeout", session), "gb sender timeout registry add");
    require(session->startup(), "gb sender timeout startup");
    io.run();

    const auto remaining = registry::instance().take_sender_session(stream->name(), "timeout");
    require(!remaining, "gb sender timeout unregisters session");
    clear_state();
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::registry::init();
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

    run("receiver_identity_is_reusable_after_shutdown", media_server::test_receiver_identity_is_reusable_after_shutdown);
    run("sender_identity_is_reusable_after_shutdown", media_server::test_sender_identity_is_reusable_after_shutdown);
    run("tcp_receiver_repeated_shutdown_is_idempotent", media_server::test_tcp_receiver_repeated_shutdown_is_idempotent);
    run("tcp_sender_repeated_shutdown_is_idempotent", media_server::test_tcp_sender_repeated_shutdown_is_idempotent);
    run("tcp_timeout_unregisters_receiver_session", media_server::test_tcp_timeout_unregisters_receiver_session);
    run("tcp_timeout_unregisters_sender_session", media_server::test_tcp_timeout_unregisters_sender_session);
    media_server::registry::destroy();
    return failures == 0 ? 0 : 1;
}
