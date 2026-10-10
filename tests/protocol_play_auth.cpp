#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "config.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "flv-muxer.h"
#include "rtmp-client.h"
#include "rtmp-server.h"
#include "rtsp-muxer.h"
}

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

struct mux_observations
{
    unsigned attempts{};
    unsigned live{};
    unsigned destroyed{};
    unsigned failures{};
    bool fail_next{};
};

mux_observations flv_mux;
mux_observations rtsp_mux;
unsigned successful_play_starts{};
unsigned sent_audio_packets{};
bool config_before_play_start{};

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct rtmp_peer
{
    std::vector<std::uint8_t> outgoing;
    unsigned audio_packets{};
};

void admission(bool rtmp, const std::string& scenario)
{
    flv_mux = {};
    rtsp_mux = {};
    successful_play_starts = 0;
    sent_audio_packets = 0;
    config_before_play_start = false;
    auto& observed = rtmp ? flv_mux : rtsp_mux;
    worker_context worker;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor signaling_listener(worker.io(), {address, 0});
    tcp::acceptor media_listener(worker.io(), {address, 0});
    const auto media_port = media_listener.local_endpoint().port();
    tcp::socket client_socket(worker.io());
    config application_config;
    application_config.signaling_url =
        *ada::parse<ada::url_aggregator>("http://127.0.0.1:" + std::to_string(signaling_listener.local_endpoint().port()));
    const std::string stream_id(64, 'a');
    const std::string token(64, 'b');
    std::optional<std::string> pending_token{token};
    auto source = std::make_shared<media_stream>(stream_id, worker);
    const auto track = media_track{.id = 1,
                                   .kind = media_kind::audio,
                                   .codec = rtmp ? codec_id::opus : codec_id::g711a,
                                   .clock_rate = scenario == "no_tracks" ? 16'000U
                                                 : rtmp                  ? 48'000U
                                                                         : 8'000U,
                                   .channel_count = 1,
                                   .codec_config = {}};
    require(source->set_tracks({track}) && stream_registry::instance().add(source), "test source setup failed");
    std::shared_ptr<media_stream> replacement;
    std::vector<std::shared_ptr<session>> owners;
    std::vector<std::weak_ptr<session>> released_owners;
    unsigned verify_requests{};
    std::exception_ptr failure;
    bool finished{};
    boost::asio::steady_timer deadline(worker.io());
    const auto finish = [&]()
    {
        if (finished)
        {
            return;
        }
        finished = true;
        deadline.cancel();
        signaling_listener.close();
        media_listener.close();
        boost::system::error_code ignored;
        client_socket.close(ignored);
        for (const auto& owner : owners)
        {
            owner->shutdown();
        }
        owners.clear();
        stream_registry::instance().remove(*source);
        source->end();
        if (replacement)
        {
            stream_registry::instance().remove(*replacement);
            replacement->end();
        }
        worker.request_stop();
    };
    const auto completed = [&](std::exception_ptr error)
    {
        if (error && !failure)
        {
            failure = error;
            finish();
        }
    };
    deadline.expires_after(5s);
    deadline.async_wait(
        [&](boost::system::error_code error)
        {
            if (!error)
            {
                failure = std::make_exception_ptr(std::runtime_error("play admission exceeded deadline"));
                finish();
            }
        });
    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            for (;;)
            {
                tcp::socket socket(worker.io());
                boost::system::error_code error;
                signaling_listener.async_accept(socket, yield[error]);
                if (error == boost::asio::error::operation_aborted)
                {
                    return;
                }
                require(!error, "verification accept failed");
                boost::beast::flat_buffer buffer;
                http::request<http::string_body> request;
                http::async_read(socket, buffer, request, yield[error]);
                require(!error && request.target() == "/internal/verify", "invalid verification request");
                const auto body = boost::json::parse(request.body()).as_object();
                require(body.at("token").as_string() == token && body.at("stream_id").as_string() == stream_id &&
                            body.at("operation").as_string() == "play",
                        "verification identity changed");
                ++verify_requests;
                if (verify_requests == 1)
                {
                    require(observed.attempts == 0, "play mux created before authorization completed");
                }
                const bool authorized = scenario != "rejected" && pending_token.has_value();
                if (authorized)
                {
                    pending_token.reset();
                    if (scenario == "factory_failure")
                    {
                        observed.fail_next = true;
                    }
                    if (scenario == "replacement")
                    {
                        stream_registry::instance().remove(*source);
                        source->end();
                        replacement = std::make_shared<media_stream>(stream_id, worker);
                        require(replacement->set_tracks({track}) && stream_registry::instance().add(replacement), "replacement setup failed");
                    }
                }
                http::response<http::empty_body> response(authorized ? http::status::ok : http::status::forbidden, 11);
                response.prepare_payload();
                http::async_write(socket, response, yield[error]);
                require(!error, "verification response failed");
            }
        },
        completed);
    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            for (;;)
            {
                tcp::socket socket(worker.io());
                boost::system::error_code error;
                media_listener.async_accept(socket, yield[error]);
                if (error == boost::asio::error::operation_aborted)
                {
                    return;
                }
                require(!error, "media accept failed");
                std::shared_ptr<session> owner;
                if (rtmp)
                {
                    auto connection = std::make_shared<rtmp_session>(worker, std::move(socket), application_config);
                    owner = connection;
                    connection->startup();
                }
                else
                {
                    auto connection = std::make_shared<rtsp_server_connection>(worker, std::move(socket), application_config);
                    owner = connection;
                    connection->startup();
                }
                released_owners.emplace_back(owner);
                owners.push_back(std::move(owner));
            }
        },
        completed);
    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            const unsigned attempts = scenario == "factory_failure" ? 2U : 1U;
            for (unsigned attempt = 0; attempt < attempts; ++attempt)
            {
                client_socket = tcp::socket(worker.io());
                boost::system::error_code error;
                client_socket.async_connect({address, media_port}, yield[error]);
                require(!error, "media client connect failed");
                if (rtmp)
                {
                    rtmp_peer peer;
                    rtmp_client_handler_t handler{};
                    handler.send = [](void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
                    {
                        auto& outgoing = static_cast<rtmp_peer*>(param)->outgoing;
                        if (header_bytes != 0)
                        {
                            const auto* data = static_cast<const std::uint8_t*>(header);
                            outgoing.insert(outgoing.end(), data, data + header_bytes);
                        }
                        if (payload_bytes != 0)
                        {
                            const auto* data = static_cast<const std::uint8_t*>(payload);
                            outgoing.insert(outgoing.end(), data, data + payload_bytes);
                        }
                        return static_cast<int>(header_bytes + payload_bytes);
                    };
                    handler.onaudio = [](void* param, const void*, std::size_t, std::uint32_t)
                    {
                        ++static_cast<rtmp_peer*>(param)->audio_packets;
                        return 0;
                    };
                    handler.onvideo = [](void*, const void*, std::size_t, std::uint32_t) { return 0; };
                    handler.onscript = [](void*, const void*, std::size_t, std::uint32_t) { return 0; };
                    const auto tc_url = "rtmp://127.0.0.1:" + std::to_string(media_port) + "/" + stream_id;
                    std::unique_ptr<rtmp_client_t, decltype(&rtmp_client_destroy)> client(
                        rtmp_client_create(stream_id.c_str(), token.c_str(), tc_url.c_str(), &peer, &handler), &rtmp_client_destroy);
                    require(client != nullptr && rtmp_client_start(client.get(), 2) == 0, "RTMP client startup failed");
                    std::array<std::uint8_t, 16'384> input{};
                    for (;;)
                    {
                        if (!peer.outgoing.empty())
                        {
                            const auto outgoing = std::exchange(peer.outgoing, {});
                            boost::asio::async_write(client_socket, boost::asio::buffer(outgoing), yield[error]);
                            require(!error, "RTMP request write failed");
                        }
                        const auto bytes = client_socket.async_read_some(boost::asio::buffer(input), yield[error]);
                        if (error)
                        {
                            require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset,
                                    "unexpected RTMP socket error");
                            break;
                        }
                        const auto result = rtmp_client_input(client.get(), input.data(), bytes);
                        if (scenario == "accepted" && peer.audio_packets != 0)
                        {
                            require(result == 0, "accepted RTMP response failed");
                            require(rtmp_client_getstate(client.get()) == 4, "RTMP config arrived before client Play.Start");
                            break;
                        }
                        if (result != 0)
                        {
                            require(scenario != "accepted", "accepted RTMP client failed");
                            break;
                        }
                    }
                    require((peer.audio_packets != 0) == (scenario == "accepted"), "RTMP media admission differs from authorization");
                }
                else
                {
                    const auto request = "DESCRIBE rtsp://127.0.0.1:" + std::to_string(media_port) + "/" + stream_id + "/" + token +
                                         " RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n";
                    boost::asio::async_write(client_socket, boost::asio::buffer(request), yield[error]);
                    require(!error, "RTSP DESCRIBE write failed");
                    boost::asio::streambuf response;
                    boost::asio::async_read_until(client_socket, response, "\r\n\r\n", yield[error]);
                    const std::string headers(boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data()));
                    if (scenario == "accepted")
                    {
                        require(!error && headers.starts_with("RTSP/1.0 200"), "accepted RTSP DESCRIBE failed");
                    }
                    else if (scenario == "replacement")
                    {
                        require(!error && headers.starts_with("RTSP/1.0 404"), "RTSP switched to replacement source during verification");
                    }
                    else
                    {
                        require(!headers.starts_with("RTSP/1.0 200") &&
                                    (!error || error == boost::asio::error::eof || error == boost::asio::error::connection_reset),
                                "failed RTSP admission emitted a successful SDP");
                    }
                }
                client_socket.close();
            }
            require(verify_requests == attempts, "verification count changed");
            require(pending_token.has_value() == (scenario == "rejected"), "authorization failure restored or consumed token incorrectly");
            require(observed.attempts == (scenario == "rejected" ? 0U : 1U), "rejection or replay created a play mux");
            if (scenario == "factory_failure")
            {
                require(!observed.fail_next && observed.failures == 1 && observed.live == 0,
                        "post-authorization factory failure was not exercised exactly once");
            }
            if (rtmp)
            {
                require(!config_before_play_start, "RTMP codec config preceded Play.Start");
                require(successful_play_starts == (scenario == "accepted" ? 1U : 0U), "failed RTMP preparation sent Play.Start");
                require(sent_audio_packets == (scenario == "accepted" ? 1U : 0U), "failed RTMP admission sent codec config");
            }
            finish();
        },
        completed);
    worker.io().run_for(6s);
    if (!finished)
    {
        finish();
        worker.io().restart();
        worker.io().run_for(1s);
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    require(observed.live == 0, "play mux remained after shutdown");
    require(observed.destroyed == (scenario == "accepted" || scenario == "replacement" || scenario == "no_tracks" ? 1U : 0U),
            "play mux cleanup count changed");
    for (const auto& owner : released_owners)
    {
        require(owner.expired(), "play connection retained after shutdown");
    }
    std::cout << (rtmp ? "RTMP " : "RTSP ") << scenario << ": PASS\n";
}
}    // namespace

extern "C" flv_muxer_t* __real_flv_muxer_create(flv_muxer_handler handler, void* param);
extern "C" void __real_flv_muxer_destroy(flv_muxer_t* muxer);
extern "C" rtsp_muxer_t* __real_rtsp_muxer_create(rtsp_muxer_onpacket handler, void* param);
extern "C" int __real_rtsp_muxer_destroy(rtsp_muxer_t* muxer);
extern "C" int __real_rtmp_server_start(rtmp_server_t* server, int code, const char* message);
extern "C" int __real_rtmp_server_send_audio(rtmp_server_t* server, const void* data, std::size_t bytes, std::uint32_t timestamp);

extern "C" flv_muxer_t* __wrap_flv_muxer_create(flv_muxer_handler handler, void* param)
{
    ++flv_mux.attempts;
    if (std::exchange(flv_mux.fail_next, false))
    {
        ++flv_mux.failures;
        return nullptr;
    }
    auto* muxer = __real_flv_muxer_create(handler, param);
    if (muxer != nullptr)
    {
        ++flv_mux.live;
    }
    return muxer;
}

extern "C" void __wrap_flv_muxer_destroy(flv_muxer_t* muxer)
{
    --flv_mux.live;
    ++flv_mux.destroyed;
    __real_flv_muxer_destroy(muxer);
}

extern "C" rtsp_muxer_t* __wrap_rtsp_muxer_create(rtsp_muxer_onpacket handler, void* param)
{
    ++rtsp_mux.attempts;
    if (std::exchange(rtsp_mux.fail_next, false))
    {
        ++rtsp_mux.failures;
        return nullptr;
    }
    auto* muxer = __real_rtsp_muxer_create(handler, param);
    if (muxer != nullptr)
    {
        ++rtsp_mux.live;
    }
    return muxer;
}

extern "C" int __wrap_rtsp_muxer_destroy(rtsp_muxer_t* muxer)
{
    --rtsp_mux.live;
    ++rtsp_mux.destroyed;
    return __real_rtsp_muxer_destroy(muxer);
}

extern "C" int __wrap_rtmp_server_start(rtmp_server_t* server, int code, const char* message)
{
    if (code == 0)
    {
        ++successful_play_starts;
    }
    return __real_rtmp_server_start(server, code, message);
}

extern "C" int __wrap_rtmp_server_send_audio(rtmp_server_t* server, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    ++sent_audio_packets;
    config_before_play_start = config_before_play_start || successful_play_starts == 0;
    return __real_rtmp_server_send_audio(server, data, bytes, timestamp);
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2 && (std::string_view(argv[1]) == "rtmp" || std::string_view(argv[1]) == "rtsp"), "expected rtmp or rtsp");
        const bool rtmp = std::string_view(argv[1]) == "rtmp";
        for (const auto* scenario : {"rejected", "factory_failure", "accepted"})
        {
            admission(rtmp, scenario);
        }
        if (!rtmp)
        {
            admission(false, "replacement");
            admission(false, "no_tracks");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "protocol play auth: " << error.what() << '\n';
        return 1;
    }
}
