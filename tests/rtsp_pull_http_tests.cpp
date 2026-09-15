#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/json.hpp>
#include <boost/url/parse.hpp>

#include "media/core/stream_registry.h"
#include "media/http/rtsp_pull_http.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace media_server
{
namespace
{

constexpr char stream_id_a[] = "550e8400-e29b-41d4-a716-446655440000";
constexpr char stream_id_b[] = "550e8400-e29b-41d4-b716-446655440001";
constexpr char source_id[] = "10000000-0000-4000-8000-000000000001";

class foreign_receiver_session final : public stream_session
{
   public:
    void shutdown(runtime_end_reason = runtime_end_reason::requested, std::string = {}) override {}
};

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string{message});
    }
}

rtsp_pull_http_request request(std::string target,
                               std::string body,
                               boost::beast::http::verb method = boost::beast::http::verb::post,
                               std::string_view content_type = "application/json")
{
    rtsp_pull_http_request value{method, std::move(target), 11};
    if (!content_type.empty())
    {
        value.set(boost::beast::http::field::content_type, content_type);
    }
    value.keep_alive(true);
    value.body() = std::move(body);
    value.prepare_payload();
    return value;
}

rtsp_pull_http_request request(std::string target, boost::json::object body)
{
    return request(std::move(target), boost::json::serialize(body));
}

rtsp_pull_http_response handle(worker_context& worker, const rtsp_pull_http_request& value)
{
    const auto target = boost::urls::parse_origin_form(value.target());
    require(target.has_value(), "rtsp pull test target");
    return handle_rtsp_pull_request(value, worker, *target);
}

void require_response(const rtsp_pull_http_response& response,
                      boost::beast::http::status status,
                      std::string_view body,
                      std::string_view message)
{
    if (response.result() != status)
    {
        throw std::runtime_error(std::string{message} + ": status " + std::to_string(response.result_int()));
    }
    require(response.version() == 11, message);
    require(!response.keep_alive(), message);
    require(response[boost::beast::http::field::content_type] == (body.empty() ? "" : "application/json"), message);
    require(response.body() == body, message);
}

boost::json::object create_body(std::string stream_name, std::string url, std::string_view stream_id = stream_id_a)
{
    return {{"stream_id", stream_id}, {"source_id", source_id}, {"stream_name", std::move(stream_name)}, {"url", std::move(url)}};
}

boost::json::object delete_body(std::string stream_name, std::string_view stream_id = stream_id_a)
{
    return {{"stream_id", stream_id}, {"stream_name", std::move(stream_name)}};
}

void test_request_validation()
{
    worker_context worker;
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();

    const auto valid_url = "rtsp://127.0.0.1:9/live/source";
    require_response(handle(worker, request("/rtsp/pull/create", create_body("live/no-auth", valid_url))),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull no auth create");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/no-auth"))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull no auth delete");

    auto auth = create_body("live/auth", valid_url);
    auth["username"] = "admin";
    auth["password"] = "";
    require_response(handle(worker, request("/rtsp/pull/create", std::move(auth))),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull auth create");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/auth"))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull auth delete");

    const std::string invalid_bodies[]{
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/missing-url"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","stream_name":"live/missing-source","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/password","url":"rtsp://127.0.0.1/live","password":"secret"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/password","url":"rtsp://127.0.0.1/live","username":"","password":""})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/userinfo","url":"rtsp://admin:secret@127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/scheme","url":"http://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/authority","url":"rtsp:/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/host","url":"rtsp:///live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/port","url":"rtsp://127.0.0.1:0/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/unknown","url":"rtsp://127.0.0.1/live","unknown":true})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":1,"url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/type","url":"rtsp://127.0.0.1/live","username":1})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/type","url":"rtsp://127.0.0.1/live","password":1})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"","stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"invalid","stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"100E8400-E29B-4000-8000-000000000001","stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-1000-8000-000000000001","stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-0000-000000000001","stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":null,"stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":1,"stream_name":"live/source-id","url":"rtsp://127.0.0.1/live"})",
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","source_id":"10000000-0000-4000-8000-000000000001","stream_name":"live/trailing","url":"rtsp://127.0.0.1/live"} {})",
        "{",
        "[]",
    };
    for (const auto& body : invalid_bodies)
    {
        require_response(handle(worker, request("/rtsp/pull/create", body)),
                         boost::beast::http::status::bad_request,
                         R"({"error":"invalid_request"})",
                         "rtsp pull invalid create");
    }

    require_response(handle(worker, request("/rtsp/pull/delete", R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","stream_name":""})")),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull invalid delete");
    require_response(handle(worker, request("/rtsp/pull/create?query=1", create_body("live/query", valid_url))),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull query rejected");
    require_response(handle(worker, request("/rtsp/pull/create", "{}", boost::beast::http::verb::get)),
                     boost::beast::http::status::method_not_allowed,
                     R"({"error":"method_not_allowed"})",
                     "rtsp pull method rejected");
    require_response(handle(worker, request("/rtsp/pull/create", "{}", boost::beast::http::verb::post, "text/plain")),
                     boost::beast::http::status::unsupported_media_type,
                     R"({"error":"unsupported_media_type"})",
                     "rtsp pull content type rejected");
    require_response(handle(worker, request("/rtsp/pull/missing", "{}")),
                     boost::beast::http::status::not_found,
                     R"({"error":"not_found"})",
                     "rtsp pull route rejected");

    worker.release_work();
    io.run();
    streams.clear();
}

void test_stream_id_required()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    auto identified = create_body("live/identified", "rtsp://127.0.0.1:9/live/source");
    require_response(handle(worker, request("/rtsp/pull/create", std::move(identified))),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull accepts stream id");

    require_response(handle(worker,
                            request("/rtsp/pull/create",
                                    {{"source_id", source_id},
                                     {"stream_name", "live/missing-id"},
                                     {"url", "rtsp://127.0.0.1:9/live/source"}})),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull requires stream id");
    require_response(handle(worker,
                            request("/rtsp/pull/create",
                                    create_body("live/uppercase-id",
                                                "rtsp://127.0.0.1:9/live/source",
                                                "550E8400-E29B-41D4-A716-446655440000"))),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull rejects noncanonical stream id");
    require_response(handle(worker,
                            request("/rtsp/pull/create",
                                    create_body("live/non-v4-id",
                                                "rtsp://127.0.0.1:9/live/source",
                                                "550e8400-e29b-11d4-a716-446655440000"))),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull requires version four stream id");
    require_response(handle(worker,
                            request("/rtsp/pull/create",
                                    create_body("live/non-rfc-variant",
                                                "rtsp://127.0.0.1:9/live/source",
                                                "550e8400-e29b-41d4-0716-446655440000"))),
                     boost::beast::http::status::bad_request,
                     R"({"error":"invalid_request"})",
                     "rtsp pull requires RFC 4122 stream id variant");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/identified"))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull identified delete");
    worker.release_work();
    worker.io().run();
    streams.clear();
}

void test_create_delete_recreate()
{
    worker_context worker;
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();
    const auto body = create_body("live/recreate", "rtsp://127.0.0.1:9/live/source");

    require_response(handle(worker, request("/rtsp/pull/create", body)),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull initial create");
    require_response(handle(worker, request("/rtsp/pull/create", body)),
                     boost::beast::http::status::conflict,
                     R"({"error":"conflict"})",
                     "rtsp pull duplicate create");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/recreate"))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull initial delete");
    require_response(handle(worker, request("/rtsp/pull/create", create_body("live/recreate", "rtsp://127.0.0.1:9/live/source", stream_id_b))),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull immediate recreate");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/recreate", stream_id_a))),
                     boost::beast::http::status::not_found,
                     R"({"error":"not_found"})",
                     "rtsp pull stale delete rejected");
    require_response(handle(worker, request("/rtsp/pull/create", create_body("live/recreate", "rtsp://127.0.0.1:9/live/source", stream_id_b))),
                     boost::beast::http::status::conflict,
                     R"({"error":"conflict"})",
                     "rtsp pull recreated identity retained");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/recreate", stream_id_b))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull recreated delete");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/recreate", stream_id_b))),
                     boost::beast::http::status::not_found,
                     R"({"error":"not_found"})",
                     "rtsp pull missing delete");

    worker.release_work();
    io.run();
    streams.clear();
}

void test_delete_preserves_foreign_receiver()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    auto foreign = std::make_shared<foreign_receiver_session>();
    require(streams.add_receiver_session("live/foreign", foreign), "rtsp pull foreign receiver identity");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/foreign"))),
                     boost::beast::http::status::not_found,
                     R"({"error":"not_found"})",
                     "rtsp pull delete preserves foreign receiver");
    require(streams.take_receiver_session("live/foreign") == foreign, "rtsp pull foreign receiver retained");
    streams.clear();
}

void test_delayed_shutdown_preserves_replacement()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    auto old_session = std::make_shared<rtsp_pull_session>(worker, stream_id_a, source_id, "live/replacement", "rtsp://127.0.0.1:9/live/old");
    require(streams.add_receiver_session("live/replacement", old_session), "rtsp pull old identity");
    auto removed = streams.take_receiver_session("live/replacement");
    require(removed.get() == old_session.get(), "rtsp pull old identity removed");

    auto replacement = std::make_shared<rtsp_pull_session>(worker, stream_id_b, source_id, "live/replacement", "rtsp://127.0.0.1:9/live/new");
    require(streams.add_receiver_session("live/replacement", replacement), "rtsp pull replacement identity");
    old_session->shutdown();
    worker.release_work();
    worker.io().run();
    auto retained = streams.take_receiver_session("live/replacement");
    require(retained.get() == replacement.get(), "rtsp pull delayed shutdown preserves replacement");

    replacement->shutdown();
    worker.io().restart();
    worker.io().run();
    streams.clear();
}

void test_runtime_failure_releases_identity()
{
    worker_context worker;
    auto& io = worker.io();
    auto& streams = stream_registry::instance();
    streams.clear();

    boost::asio::ip::tcp::acceptor endpoint(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = endpoint.local_endpoint().port();
    endpoint.close();
    const auto body = create_body("live/failure", "rtsp://127.0.0.1:" + std::to_string(port) + "/live/source");
    require_response(handle(worker, request("/rtsp/pull/create", body)),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull failure create");
    worker.release_work();
    io.run();
    io.restart();
    require_response(handle(worker, request("/rtsp/pull/create", body)),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull recreate after runtime failure");
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/failure"))),
                     boost::beast::http::status::no_content,
                     "",
                     "rtsp pull failure replacement delete");
    io.run();
    streams.clear();
}

void test_registry_shutdown_stops_pull()
{
    worker_context worker;
    auto& streams = stream_registry::instance();
    streams.clear();

    require_response(handle(worker,
                            request("/rtsp/pull/create", create_body("live/service-stop", "rtsp://127.0.0.1:9/live/source"))),
                     boost::beast::http::status::created,
                     "",
                     "rtsp pull service stop create");
    streams.shutdown_sessions();
    worker.release_work();
    worker.io().run();
    require_response(handle(worker, request("/rtsp/pull/delete", delete_body("live/service-stop"))),
                     boost::beast::http::status::not_found,
                     R"({"error":"not_found"})",
                     "rtsp pull service stop removes identity");
    streams.clear();
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_request_validation();
        media_server::test_stream_id_required();
        media_server::test_create_delete_recreate();
        media_server::test_delete_preserves_foreign_receiver();
        media_server::test_delayed_shutdown_preserves_replacement();
        media_server::test_runtime_failure_releases_identity();
        media_server::test_registry_shutdown_stops_pull();
        std::cout << "[pass] rtsp_pull_http\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        media_server::stream_registry::instance().clear();
        std::cerr << "[fail] rtsp_pull_http: " << error.what() << '\n';
        return 1;
    }
}
