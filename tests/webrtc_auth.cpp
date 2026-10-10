#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/json.hpp>
#include <ada.h>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/core/session_registry.h"
#include "media/http/whep_http.h"
#include "media/http/whip_http.h"
#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/whep.h"
#include "media/webrtc/whip.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void admission(bool publishing, const std::string& scenario)
{
    worker_context worker;
    tcp::acceptor listener(worker.io(), {boost::asio::ip::make_address("127.0.0.1"), 0});
    tcp::socket http_socket(worker.io());
    http_socket.open(tcp::v4());
    boost::asio::ip::udp::socket occupied_port(worker.io());
    if (scenario == "transport_failure")
    {
        occupied_port.open(boost::asio::ip::udp::v4());
        occupied_port.bind({boost::asio::ip::make_address("127.0.0.1"), 54'000});
    }
    config application_config;
    application_config.webrtc_address = "127.0.0.1";
    application_config.signaling_url = *ada::parse<ada::url_aggregator>("http://127.0.0.1:" + std::to_string(listener.local_endpoint().port()));
    const std::string stream_id(64, 'a');
    const std::string token = publishing ? stream_id : std::string(64, 'b');
    const auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "test certificate failed");
    const auto mid = std::string("0");
    const auto offer = std::string("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE ") + mid + "\r\n" +
        (publishing ? "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n" : "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\n") +
        "c=IN IP4 0.0.0.0\r\na=mid:" + mid + "\r\na=rtcp-mux\r\na=setup:actpass\r\n"
        "a=ice-ufrag:test-remote\r\na=ice-pwd:test-remote-password\r\na=fingerprint:sha-256 " + certificate->sha256_fingerprint() + "\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n" +
        (publishing ? "a=sendonly\r\na=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n" :
                      "a=recvonly\r\na=rtpmap:8 PCMA/8000\r\n");
    auto source = std::make_shared<media_stream>(stream_id, worker);
    auto track = media_track{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a,
                             .clock_rate = 8'000U, .channel_count = 1, .codec_config = {}};
    require(source->set_tracks({track}), "test tracks rejected");
    const bool source_present = !publishing && scenario != "missing_source";
    if (source_present)
    {
        require(stream_registry::instance().add(source), "test source registration failed");
    }
    std::shared_ptr<media_stream> replacement;
    std::exception_ptr failure;
    unsigned verify_requests{};
    std::string created_session_id;
    const auto completed = [&](std::exception_ptr error)
    {
        if (error)
        {
            failure = error;
            worker.request_stop();
        }
    };
    boost::asio::spawn(worker.io(), [&](boost::asio::yield_context yield)
    {
        tcp::socket socket(worker.io());
        boost::system::error_code error;
        listener.async_accept(socket, yield[error]);
        if (error == boost::asio::error::operation_aborted)
        {
            return;
        }
        require(!error, "verification accept failed");
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::async_read(socket, buffer, request, yield[error]);
        require(!error && request.target() == "/internal/verify", "invalid verification request");
        ++verify_requests;
        const auto body = boost::json::parse(request.body()).as_object();
        require(body.at("token").as_string() == token && body.at("stream_id").as_string() == stream_id &&
                body.at("operation").as_string() == (publishing ? "publish" : "play"), "verification identity changed");
        if (scenario == "closed_http")
        {
            http_socket.close();
        }
        if (scenario == "replacement")
        {
            stream_registry::instance().remove(*source);
            source->end();
            replacement = std::make_shared<media_stream>(stream_id, worker);
            require(replacement->set_tracks({track}) && stream_registry::instance().add(replacement), "replacement setup failed");
        }
        http::response<http::empty_body> response(scenario == "rejected" ? http::status::forbidden : http::status::ok, 11);
        response.prepare_payload();
        http::async_write(socket, response, yield[error]);
        require(!error, "verification response failed");
    }, completed);
    boost::asio::spawn(worker.io(), [&](boost::asio::yield_context yield)
    {
        std::string path = publishing ? "/publish/whip/" + stream_id : "/play/whep/" + stream_id + "/" + token;
        if (scenario == "invalid_path")
        {
            path += "/extra";
        }
        if (scenario == "query")
        {
            path += "?token=extra";
        }
        if (scenario == "normalized_path")
        {
            path.insert(publishing ? std::string("/publish/whip/").size() : std::string("/play/whep/").size(), "./");
        }
        if (scenario == "encoded_path")
        {
            path.replace(path.find(stream_id), 1, "%61");
        }
        if (scenario == "reserved_path")
        {
            path = publishing ? "/publish/whip/session/a/b" : "/play/whep/session/a/b";
        }
        http::request<http::string_body> request(http::verb::post, path, 11);
        request.set(http::field::content_type, "application/sdp");
        request.body() = scenario == "invalid_offer" ? "not an SDP" : offer;
        const auto target = ada::parse<ada::url_aggregator>("http://localhost" + path);
        require(static_cast<bool>(target), "invalid test target");
        const auto response = publishing ? handle_whip_request(request, worker, *target, application_config, http_socket, yield) :
                                           handle_whep_request(request, worker, *target, application_config, http_socket, yield);
        http::status expected = http::status::created;
        if (scenario == "invalid_path" || scenario == "reserved_path") expected = http::status::not_found;
        if (scenario == "query" || scenario == "invalid_offer" || scenario == "normalized_path" || scenario == "encoded_path") expected = http::status::bad_request;
        if (scenario == "missing_source") expected = http::status::conflict;
        if (scenario == "transport_failure") expected = http::status::internal_server_error;
        if (scenario == "rejected" || scenario == "closed_http") expected = http::status::forbidden;
        if (response.result() != expected)
        {
            throw std::runtime_error(scenario + " admission response " + std::to_string(response.result_int()) +
                                     " expected " + std::to_string(static_cast<unsigned>(expected)));
        }
        const bool precheck_failed = scenario == "invalid_path" || scenario == "query" || scenario == "invalid_offer" ||
            scenario == "normalized_path" || scenario == "encoded_path" || scenario == "reserved_path" ||
            scenario == "missing_source" || scenario == "transport_failure";
        require(verify_requests == (precheck_failed ? 0U : 1U), "precheck consumed authorization or verification missing");
        if (expected == http::status::created)
        {
            const auto location = response[http::field::location];
            const std::string session_id(location.substr(location.find_last_of('/') + 1));
            created_session_id = session_id;
            const std::string resource_path(location);
            const auto resource_target = ada::parse<ada::url_aggregator>("http://localhost" + resource_path);
            if (publishing)
            {
                require(session_registry::instance().find_receiver_session(stream_id) != nullptr, "verified WHIP not registered");
                http::request<http::string_body> remove(http::verb::delete_, resource_path, 11);
                require(handle_whip_request(remove, worker, *resource_target, application_config, http_socket, yield).result() == http::status::no_content,
                        "WHIP resource DELETE failed");
            }
            else
            {
                if (scenario == "replacement")
                {
                    // 源终止通过 owner executor 关闭 socket，等整个 executor drain 后检查释放。
                }
                else
                {
                    require(whep::contains(session_id), "WHEP resource not active");
                    for (const auto method : {http::verb::get, http::verb::head})
                    {
                        http::request<http::string_body> inspect(method, resource_path, 11);
                        require(handle_whep_request(inspect, worker, *resource_target, application_config, http_socket, yield).result() == http::status::no_content,
                                "WHEP resource inspection failed");
                    }
                    http::request<http::string_body> remove(http::verb::delete_, resource_path, 11);
                    require(handle_whep_request(remove, worker, *resource_target, application_config, http_socket, yield).result() == http::status::no_content,
                            "WHEP resource DELETE failed");
                }
            }
            require(verify_requests == 1U, "session resource unexpectedly reverified authorization");
        }
        require(!publishing || expected == http::status::created || !session_registry::instance().find_receiver_session(stream_id),
                "unauthorized WHIP became registered");
        listener.close();
        worker.request_stop();
    }, completed);
    worker.io().run_for(5s);
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    require(worker.stop_requested(), "test exceeded deadline");
    require(!session_registry::instance().find_receiver_session(stream_id), "WHIP shutdown retained registry entry");
    if (scenario == "replacement") require(!whep::contains(created_session_id), "WHEP switched to replacement source after verification");
    if (source_present) stream_registry::instance().remove(*source);
    if (replacement) stream_registry::instance().remove(*replacement);
    const auto available = media_port_pool::instance().acquire();
    require(available && *available == 54'000, "failed or stopped session leaked media port");
    media_port_pool::instance().release(*available);
    std::cout << (publishing ? "WHIP " : "WHEP ") << scenario << ": PASS\n";
}
}    // namespace

int main()
{
    try
    {
        media_port_pool::init(54'000, 54'031);
        for (const auto& scenario : {"invalid_path", "query", "normalized_path", "encoded_path", "reserved_path", "missing_source", "invalid_offer", "transport_failure", "rejected", "closed_http", "accepted", "replacement"})
        {
            admission(false, scenario);
        }
        for (const auto& scenario : {"invalid_path", "query", "normalized_path", "encoded_path", "reserved_path", "invalid_offer", "rejected", "closed_http", "accepted"})
        {
            admission(true, scenario);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
