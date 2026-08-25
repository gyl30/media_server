#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181.h"
#include "media/http/gb28181_http.h"

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
    gb28181_http_request value{boost::beast::http::verb::post, "/gb28181/input/create", 11};
    value.keep_alive(true);
    return value;
}

void require_json_response(const gb28181_http_response& response,
                           boost::beast::http::status status,
                           std::string_view body,
                           std::string_view message)
{
    require(response.result() == status, message);
    require(response.version() == 11, message);
    require(response.keep_alive(), message);
    require(response[boost::beast::http::field::content_type] == "application/json", message);
    require(response.body() == body, message);
}

gb28181_input_config make_input_config(boost::asio::io_context& io)
{
    boost::asio::ip::udp::socket rtp_probe(io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_probe(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto rtp_port = rtp_probe.local_endpoint().port();
    const auto rtcp_port = rtcp_probe.local_endpoint().port();
    return gb28181_input_config{.stream_name = "live/http-handler-input",
                                .description = gb28181_description{.transport = gb28181_transport::udp,
                                                                    .address = boost::asio::ip::address_v4::loopback(),
                                                                    .rtp_port = rtp_port,
                                                                    .rtcp_port = rtcp_port,
                                                                    .payload_type = 96,
                                                                    .ssrc = 100},
                                .remote_rtp_endpoint = std::nullopt,
                                .remote_rtcp_port = 30001};
}

gb28181_output_config make_output_config()
{
    return gb28181_output_config{.stream_name = "live/http-handler-output",
                                 .output_id = "primary",
                                 .description = gb28181_description{.transport = gb28181_transport::udp,
                                                                     .address = boost::asio::ip::address_v4::loopback(),
                                                                     .rtp_port = 32000,
                                                                     .rtcp_port = 32001,
                                                                     .payload_type = 96,
                                                                     .ssrc = 101},
                                 .rtcp = false};
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

void test_input_handlers()
{
    boost::asio::io_context io;
    registry::instance().clear();
    auto config = make_input_config(io);

    auto create_request = request();
    const auto create_response = handle_gb28181_input_create(create_request, io, config);
    require_json_response(create_response, boost::beast::http::status::created, R"({"result":"ok"})", "input create response");

    const auto duplicate_response = handle_gb28181_input_create(create_request, io, config);
    require_json_response(duplicate_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "input create failure response");

    auto delete_request = request();
    const auto delete_response = handle_gb28181_input_delete(delete_request, "live/http-handler-input");
    require_json_response(delete_response, boost::beast::http::status::ok, R"({"result":"ok"})", "input delete response");

    const auto missing_response = handle_gb28181_input_delete(delete_request, "live/http-handler-input");
    require_json_response(missing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "input delete failure response");
}

void test_output_handlers()
{
    boost::asio::io_context io;
    registry::instance().clear();
    auto stream = std::make_shared<media_stream>("live/http-handler-output", io.get_executor());
    require(stream->set_tracks({make_video_track()}), "output handler tracks");
    require(registry::instance().add(stream), "output handler stream");
    auto config = make_output_config();

    auto create_request = request();
    const auto create_response = handle_gb28181_output_create(create_request, io, config);
    require_json_response(create_response, boost::beast::http::status::created, R"({"result":"ok"})", "output create response");

    const auto duplicate_response = handle_gb28181_output_create(create_request, io, config);
    require_json_response(duplicate_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "output create failure response");

    auto delete_request = request();
    const auto delete_response = handle_gb28181_output_delete(delete_request, "live/http-handler-output", "primary");
    require_json_response(delete_response, boost::beast::http::status::ok, R"({"result":"ok"})", "output delete response");

    const auto missing_response = handle_gb28181_output_delete(delete_request, "live/http-handler-output", "primary");
    require_json_response(missing_response,
                          boost::beast::http::status::internal_server_error,
                          R"({"error":"operation_failed"})",
                          "output delete failure response");
    registry::instance().clear();
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_input_handlers();
        media_server::test_output_handlers();
        std::cout << "[pass] gb28181_http_handlers\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] gb28181_http_handlers: " << error.what() << '\n';
        return 1;
    }
}
