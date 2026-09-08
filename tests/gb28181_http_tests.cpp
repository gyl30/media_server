#include <string>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/json.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/core/media_stream.h"
#include "media/http/gb28181_http.h"
#include "media/core/stream_registry.h"
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"

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

gb28181_http_request request(std::string target, boost::json::object body)
{
    gb28181_http_request value{boost::beast::http::verb::post, std::move(target), 11};
    value.set(boost::beast::http::field::content_type, "application/json");
    value.keep_alive(true);
    value.body() = boost::json::serialize(body);
    value.prepare_payload();
    return value;
}

void require_json_response(const gb28181_http_response& response, boost::beast::http::status status, std::string_view body, std::string_view message)
{
    require(response.result() == status, message);
    require(response.version() == 11, message);
    require(!response.keep_alive(), message);
    require(response[boost::beast::http::field::content_type] == "application/json", message);
    require(response.body() == body, message);
}

gb28181_http_response receiver_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "receiver request target");
    return handle_gb28181_receiver_request(request, worker, *target, boost::asio::ip::address_v4::loopback());
}

gb28181_http_response sender_request(worker_context& worker, gb28181_http_request request)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "sender request target");
    return handle_gb28181_sender_request(request, worker, *target, boost::asio::ip::address_v4::loopback());
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

void test_receiver_handlers()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    registry::instance().clear();
    boost::json::object create_body;
    create_body["stream_name"] = "live/http-handler-receiver";
    create_body["transport"] = "udp";
    create_body["payload_type"] = 96;
    create_body["ssrc"] = 100;

    const auto create_response = receiver_request(worker, request("/gb28181/receiver/create", create_body));
    require(create_response.result() == boost::beast::http::status::created, "receiver create response status");
    const auto create_result = boost::json::parse(create_response.body()).as_object();
    require(create_result.at("result").as_string() == "ok", "receiver create response result");
    const auto rtp_port = static_cast<std::uint16_t>(create_result.at("rtp_port").as_int64());
    const auto rtcp_port = static_cast<std::uint16_t>(create_result.at("rtcp_port").as_int64());
    require(rtp_port != 0 && (rtp_port & 1U) == 0U && rtcp_port == rtp_port + 1U, "receiver create response port pair");

    boost::system::error_code bind_error;
    boost::asio::ip::udp::socket rtp_probe(io);
    rtp_probe.open(boost::asio::ip::udp::v4());
    rtp_probe.bind({boost::asio::ip::address_v4::loopback(), rtp_port}, bind_error);
    require(bind_error == boost::asio::error::address_in_use, "receiver create binds rtp port before response");
    boost::asio::ip::udp::socket rtcp_probe(io);
    rtcp_probe.open(boost::asio::ip::udp::v4());
    rtcp_probe.bind({boost::asio::ip::address_v4::loopback(), rtcp_port}, bind_error);
    require(bind_error == boost::asio::error::address_in_use, "receiver create binds rtcp port before response");

    boost::asio::ip::udp::socket other_address_probe(io);
    other_address_probe.open(boost::asio::ip::udp::v4());
    other_address_probe.bind({boost::asio::ip::make_address_v4("127.0.0.2"), rtp_port}, bind_error);
    require(!bind_error, "receiver create only binds configured local address");

    const auto duplicate_response = receiver_request(worker, request("/gb28181/receiver/create", create_body));
    require_json_response(
        duplicate_response, boost::beast::http::status::internal_server_error, R"({"error":"operation_failed"})", "receiver create failure response");

    boost::json::object delete_body;
    delete_body["stream_name"] = "live/http-handler-receiver";
    const auto delete_response = receiver_request(worker, request("/gb28181/receiver/delete", delete_body));
    require_json_response(delete_response, boost::beast::http::status::ok, R"({"result":"ok"})", "receiver delete response");

    const auto closing_response = receiver_request(worker, request("/gb28181/receiver/delete", delete_body));
    require_json_response(closing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "receiver delete after identity release response");
    io.run();
    io.restart();

    const auto released = port_manager::instance().acquire_pair();
    require(released && released->first == rtp_port && released->second == rtcp_port, "receiver delete releases returned port pair");
    port_manager::instance().release(*released);

    const auto missing_response = receiver_request(worker, request("/gb28181/receiver/delete", delete_body));
    require_json_response(missing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "receiver delete after shutdown response");
    registry::instance().clear();
}

void test_sender_handlers()
{
    worker_context worker;
    worker.release_work();
    worker.io().restart();
    auto& io = worker.io();
    registry::instance().clear();
    auto stream = std::make_shared<media_stream>("live/http-handler-sender", worker);
    require(stream->set_tracks({make_video_track()}), "sender handler tracks");
    require(registry::instance().add(stream), "sender handler stream");

    boost::json::object create_body;
    create_body["stream_name"] = stream->name();
    create_body["sender_id"] = "primary";
    create_body["transport"] = "udp";
    create_body["remote_address"] = "127.0.0.1";
    create_body["remote_rtp_port"] = 32000;
    create_body["payload_type"] = 96;
    create_body["ssrc"] = 101;

    const auto create_response = sender_request(worker, request("/gb28181/sender/create", create_body));
    require_json_response(create_response, boost::beast::http::status::created, R"({"result":"ok"})", "sender create response");

    const auto duplicate_response = sender_request(worker, request("/gb28181/sender/create", create_body));
    require_json_response(
        duplicate_response, boost::beast::http::status::internal_server_error, R"({"error":"operation_failed"})", "sender create failure response");

    boost::json::object delete_body;
    delete_body["stream_name"] = stream->name();
    delete_body["sender_id"] = "primary";
    const auto delete_response = sender_request(worker, request("/gb28181/sender/delete", delete_body));
    require_json_response(delete_response, boost::beast::http::status::ok, R"({"result":"ok"})", "sender delete response");

    const auto closing_response = sender_request(worker, request("/gb28181/sender/delete", delete_body));
    require_json_response(closing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "sender delete after identity release response");
    io.run();
    io.restart();

    const auto missing_response = sender_request(worker, request("/gb28181/sender/delete", delete_body));
    require_json_response(missing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "sender delete after shutdown response");
    registry::instance().clear();
}

void test_request_namespace_dispatch()
{
    worker_context worker;
    worker.release_work();

    const auto receiver_response = receiver_request(worker, request("/gb28181/receiver/missing", {}));
    require_json_response(receiver_response, boost::beast::http::status::not_found, R"({"error":"not_found"})", "receiver request route");

    const auto sender_response = sender_request(worker, request("/gb28181/sender/missing", {}));
    require_json_response(sender_response, boost::beast::http::status::not_found, R"({"error":"not_found"})", "sender request route");
}

}    // namespace
}    // namespace media_server

int main()
{
    media_server::port_manager::init(media_server::default_media_port_start, media_server::default_media_port_end);
    media_server::registry::init();
    try
    {
        media_server::test_receiver_handlers();
        media_server::test_sender_handlers();
        media_server::test_request_namespace_dispatch();
        std::cout << "[pass] gb28181_http_handlers\n";
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::registry::destroy();
        media_server::port_manager::destroy();
        std::cerr << "[fail] gb28181_http_handlers: " << error.what() << '\n';
        return 1;
    }
}
