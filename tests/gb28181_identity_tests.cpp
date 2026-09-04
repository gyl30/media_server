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
#include "media/gb28181/gb28181_tcp_session.h"
#include "media/gb28181/gb28181_tcp_output_session.h"

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

gb28181_http_response input_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "gb input target");
    return handle_gb28181_input_request(request, worker, *target);
}

gb28181_http_response output_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "gb output target");
    return handle_gb28181_output_request(request, worker, *target, boost::asio::ip::address_v4::loopback());
}

gb28181_description make_tcp_active_description(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_description{.transport = gb28181_transport::tcp_active,
                               .address = boost::asio::ip::address_v4::loopback(),
                               .rtp_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
}

gb28181_description make_tcp_passive_description(std::uint16_t port, std::uint32_t ssrc)
{
    return gb28181_description{.transport = gb28181_transport::tcp_passive,
                               .address = boost::asio::ip::address_v4::loopback(),
                               .rtp_port = port,
                               .payload_type = 96,
                               .ssrc = ssrc};
}

gb28181_http_response create_input(worker_context& worker, std::string_view stream_name, const gb28181_description& description)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    body["transport"] = "tcp_active";
    body["address"] = description.address.to_string();
    body["rtp_port"] = description.rtp_port;
    body["payload_type"] = description.payload_type;
    body["ssrc"] = description.ssrc;
    return input_request(worker, request("/gb28181/create", std::move(body)));
}

gb28181_http_response delete_input(worker_context& worker, std::string_view stream_name)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    return input_request(worker, request("/gb28181/delete", std::move(body)));
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
    require(stream->set_tracks({make_video_track()}), "gb output tracks");
    require(registry::instance().add(stream), "gb output registry");
    return stream;
}

gb28181_http_response create_output(worker_context& worker,
                                    const media_stream& stream,
                                    std::string_view output_id,
                                    const gb28181_description& description)
{
    boost::json::object body;
    body["stream_name"] = stream.name();
    body["output_id"] = output_id;
    body["transport"] = "tcp_active";
    body["address"] = description.address.to_string();
    body["rtp_port"] = description.rtp_port;
    body["payload_type"] = description.payload_type;
    body["ssrc"] = description.ssrc;
    return output_request(worker, request("/play/gb28181/create", std::move(body)));
}

gb28181_http_response delete_output(worker_context& worker, std::string_view stream_name, std::string_view output_id)
{
    boost::json::object body;
    body["stream_name"] = stream_name;
    body["output_id"] = output_id;
    return output_request(worker, request("/play/gb28181/delete", std::move(body)));
}

void clear_state() { registry::instance().clear(); }

void test_input_identity_is_reusable_after_shutdown()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto description = make_tcp_active_description(65'000, 10'000'2001);

    require_status(create_input(worker, "live/gb-identity", description), boost::beast::http::status::created, "gb input first create");
    require_status(delete_input(worker, "live/gb-identity"), boost::beast::http::status::ok, "gb input remove");
    require_status(create_input(worker, "live/gb-identity", description), boost::beast::http::status::created, "gb input reusable after shutdown");
    require_status(delete_input(worker, "live/gb-identity"), boost::beast::http::status::ok, "gb input final remove");
    io.run();
    clear_state();
}

void test_output_identity_is_reusable_after_shutdown()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-identity");
    const auto description = make_tcp_active_description(65'000, 10'000'2002);

    require_status(create_output(worker, *stream, "primary", description), boost::beast::http::status::created, "gb output first create");
    require_status(delete_output(worker, stream->name(), "primary"), boost::beast::http::status::ok, "gb output remove");
    require_status(create_output(worker, *stream, "primary", description), boost::beast::http::status::created, "gb output reusable after shutdown");
    require_status(delete_output(worker, stream->name(), "primary"), boost::beast::http::status::ok, "gb output final remove");
    io.run();
    clear_state();
}

void test_tcp_input_repeated_shutdown_is_idempotent()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const std::string stream_name = "live/gb-input-repeated-shutdown";
    const auto description = make_tcp_passive_description(0, 10'000'2007);
    auto session = std::make_shared<gb28181_tcp_session>(worker, stream_name, description, std::chrono::seconds(1));
    require(registry::instance().add_input_session(stream_name, session), "gb input repeated shutdown registry add");
    require(session->startup(), "gb input repeated shutdown startup");

    session->shutdown();
    session->shutdown();
    session->shutdown();
    io.run();

    const auto remaining = registry::instance().take_input_session(stream_name);
    require(!remaining, "gb input repeated shutdown unregisters session");
    clear_state();
}

void test_tcp_output_repeated_shutdown_is_idempotent()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-repeated-shutdown");
    const auto description = make_tcp_passive_description(0, 10'000'2008);
    auto session = std::make_shared<gb28181_tcp_output_session>(worker,
                                                                std::weak_ptr<media_stream>{stream},
                                                                stream->name(),
                                                                "repeated-shutdown",
                                                                description,
                                                                std::chrono::seconds(1));
    require(registry::instance().add_output_session(stream->name(), "repeated-shutdown", session), "gb output repeated shutdown registry add");
    require(session->startup(), "gb output repeated shutdown startup");

    session->shutdown();
    session->shutdown();
    session->shutdown();
    io.run();

    const auto remaining = registry::instance().take_output_session(stream->name(), "repeated-shutdown");
    require(!remaining, "gb output repeated shutdown unregisters session");
    clear_state();
}

void test_tcp_timeout_unregisters_input_session()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const std::string stream_name = "live/gb-input-timeout";
    const auto description = make_tcp_passive_description(0, 10'000'2005);
    auto session = std::make_shared<gb28181_tcp_session>(worker, stream_name, description, std::chrono::milliseconds(5));
    require(registry::instance().add_input_session(stream_name, session), "gb input timeout registry add");
    require(session->startup(), "gb input timeout startup");
    io.run();

    const auto remaining = registry::instance().take_input_session(stream_name);
    require(!remaining, "gb input timeout unregisters session");
    clear_state();
}

void test_tcp_timeout_unregisters_output_session()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    clear_state();
    const auto stream = add_video_stream(io, "live/gb-output-timeout");
    const auto description = make_tcp_passive_description(0, 10'000'2006);
    auto session = std::make_shared<gb28181_tcp_output_session>(worker,
                                                                std::weak_ptr<media_stream>{stream},
                                                                stream->name(),
                                                                "timeout",
                                                                description,
                                                                std::chrono::milliseconds(5));
    require(registry::instance().add_output_session(stream->name(), "timeout", session), "gb output timeout registry add");
    require(session->startup(), "gb output timeout startup");
    io.run();

    const auto remaining = registry::instance().take_output_session(stream->name(), "timeout");
    require(!remaining, "gb output timeout unregisters session");
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

    run("input_identity_is_reusable_after_shutdown", media_server::test_input_identity_is_reusable_after_shutdown);
    run("output_identity_is_reusable_after_shutdown", media_server::test_output_identity_is_reusable_after_shutdown);
    run("tcp_input_repeated_shutdown_is_idempotent", media_server::test_tcp_input_repeated_shutdown_is_idempotent);
    run("tcp_output_repeated_shutdown_is_idempotent", media_server::test_tcp_output_repeated_shutdown_is_idempotent);
    run("tcp_timeout_unregisters_input_session", media_server::test_tcp_timeout_unregisters_input_session);
    run("tcp_timeout_unregisters_output_session", media_server::test_tcp_timeout_unregisters_output_session);
    media_server::registry::destroy();
    return failures == 0 ? 0 : 1;
}
