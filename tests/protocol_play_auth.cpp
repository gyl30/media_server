#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <utility>
#include <iostream>
#include <optional>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <boost/json.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/buffers_iterator.hpp>

#include "config.h"
#include "media/core/media_stream.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtmp/rtmp_play_session.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "flv-muxer.h"
#include "rtsp-muxer.h"
#include "rtmp-client.h"
#include "rtmp-server.h"
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
unsigned invalid_context_sends{};
std::shared_ptr<media_stream> late_frame_source;
std::shared_ptr<boost::asio::steady_timer> late_frame_completion;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string take_rtsp_response(tcp::socket& socket, boost::asio::streambuf& buffer, const boost::asio::yield_context& yield)
{
    boost::system::error_code error;
    const auto header_bytes = boost::asio::async_read_until(socket, buffer, "\r\n\r\n", yield[error]);
    require(!error, "RTSP response read failed");
    std::string headers(boost::asio::buffers_begin(buffer.data()),
                        boost::asio::buffers_begin(buffer.data()) + static_cast<std::ptrdiff_t>(header_bytes));
    buffer.consume(header_bytes);
    std::size_t body_bytes{};
    if (const auto position = headers.find("Content-Length: "); position != std::string::npos)
    {
        body_bytes = std::stoul(headers.substr(position + 16));
    }
    if (buffer.size() < body_bytes)
    {
        boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(body_bytes - buffer.size()), yield[error]);
        require(!error, "RTSP response body read failed");
    }
    buffer.consume(body_bytes);
    return headers;
}

std::string read_until_closed(tcp::socket& socket, boost::asio::streambuf& buffer, const boost::asio::yield_context& yield)
{
    boost::system::error_code error;
    boost::asio::async_read(socket, buffer, yield[error]);
    require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset, "RTSP play connection was not closed");
    return {boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data())};
}

struct rtmp_peer
{
    std::vector<std::uint8_t> outgoing;
    unsigned audio_packets{};
};

void player_callback_release()
{
    worker_context worker;
    auto source = std::make_shared<media_stream>("closed-player", worker);
    auto owner = std::make_shared<int>(42);
    const std::weak_ptr<int> released_owner = owner;
    auto player = std::make_shared<rtmp_play_session>(
        worker, source, [owner](int, std::span<const std::uint8_t>, std::uint32_t) { return *owner - 42; }, [owner]() { ++*owner; });
    owner.reset();
    player->shutdown();
    player->shutdown();
    worker.io().poll();
    require(released_owner.expired(), "closed RTMP player retained parent callbacks");
    player->on_end();
    player->on_frame({.track = 1, .payload = std::make_shared<const std::vector<std::uint8_t>>(1, 0)});
    player.reset();
    worker.request_stop();
    worker.run();
    std::cout << "RTMP retained closed player releases parent callbacks: PASS\n";
}

void admission(bool rtmp, const std::string& scenario)
{
    flv_mux = {};
    rtsp_mux = {};
    successful_play_starts = 0;
    sent_audio_packets = 0;
    config_before_play_start = false;
    invalid_context_sends = 0;
    const bool accepted = scenario == "accepted" || scenario == "late_frame" || scenario.starts_with("play_");
    const bool cross_worker = scenario == "play_cross_worker";
    auto& observed = rtmp ? flv_mux : rtsp_mux;
    worker_context worker;
    worker_context source_worker;
    std::thread source_thread;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor signaling_listener(worker.io(), {address, 0});
    tcp::acceptor media_listener(worker.io(), {address, 0});
    const auto media_port = media_listener.local_endpoint().port();
    tcp::socket client_socket(worker.io());
    config application_config;
    application_config.signaling_url =
        *ada::parse<ada::url_aggregator>("http://127.0.0.1:" + std::to_string(signaling_listener.local_endpoint().port()));
    static unsigned invocation{};
    std::string stream_id(64, 'a');
    const auto suffix = std::to_string(++invocation);
    stream_id.replace(stream_id.size() - suffix.size(), suffix.size(), suffix);
    const std::string token(64, 'b');
    std::optional<std::string> pending_token{token};
    auto source = std::make_shared<media_stream>(stream_id, cross_worker ? source_worker : worker);
    const auto track = media_track{.id = 1,
                                   .kind = media_kind::audio,
                                   .codec = rtmp ? codec_id::opus : codec_id::g711a,
                                   .clock_rate = scenario == "no_tracks" ? 16'000U
                                                 : rtmp                  ? 48'000U
                                                                         : 8'000U,
                                   .channel_count = 1,
                                   .codec_config = {}};
    require(source->set_tracks({track}) && stream_registry::instance().add(source), "test source setup failed");
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
        if (late_frame_completion)
        {
            late_frame_completion->cancel();
        }
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
        if (cross_worker)
        {
            boost::asio::post(source_worker.io(), [source]() { source->end(); });
        }
        else
        {
            source->end();
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
    const auto audio_frame = [](std::int64_t timestamp_ns)
    {
        return media_frame{
            .track = 1, .dts_ns = timestamp_ns, .pts_ns = timestamp_ns, .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5)};
    };
    const auto publish = [&](media_frame frame)
    {
        if (cross_worker)
        {
            boost::asio::post(source_worker.io(), [source, frame = std::move(frame)]() { source->publish(frame); });
            return;
        }
        source->publish(std::move(frame));
    };
    // 非毫秒对齐的 G.711 时间戳会让 rtsp_play_session::on_frame 走失败分支，依次投递子、父 shutdown。
    const auto play_after_describe = [&](tcp::socket& socket, boost::asio::streambuf& response, const boost::asio::yield_context& yield)
    {
        take_rtsp_response(socket, response, yield);
        const auto url = "rtsp://127.0.0.1:" + std::to_string(media_port) + "/" + stream_id + "/" + token;
        boost::system::error_code error;
        const auto setup_request = "SETUP " + url + "/trackID=1 RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
        boost::asio::async_write(socket, boost::asio::buffer(setup_request), yield[error]);
        require(!error, "RTSP SETUP write failed");
        const auto setup = take_rtsp_response(socket, response, yield);
        require(setup.starts_with("RTSP/1.0 200"), "RTSP SETUP failed");
        const auto session_begin = setup.find("Session: ") + 9;
        const auto session = setup.substr(session_begin, setup.find_first_of(";\r", session_begin) - session_begin);
        const auto play_request = [&](int sequence)
        { return "PLAY " + url + " RTSP/1.0\r\nCSeq: " + std::to_string(sequence) + "\r\nSession: " + session + "\r\n\r\n"; };
        boost::asio::async_write(socket, boost::asio::buffer(play_request(3)), yield[error]);
        require(!error, "RTSP PLAY write failed");
        require(take_rtsp_response(socket, response, yield).starts_with("RTSP/1.0 200"), "RTSP PLAY failed");

        if (scenario == "play_normal")
        {
            publish(audio_frame(20'000'000));
            std::array<std::uint8_t, 4> header{};
            boost::asio::async_read(socket, response, boost::asio::transfer_at_least(4), yield[error]);
            require(!error && response.size() >= 4, "RTSP interleaved read failed");
            boost::asio::buffer_copy(boost::asio::buffer(header), response.data());
            require(header[0] == 0x24 && header[1] == 0, "RTSP play sent unexpected interleaved channel");
            require(stream_registry::instance().find(stream_id) == source, "normal RTSP play changed source generation");
            return;
        }

        std::string late_output;
        if (scenario == "play_replied")
        {
            // 第二个 PLAY 已被处理并回复后，媒体才失败。
            boost::asio::async_write(socket, boost::asio::buffer(play_request(4)), yield[error]);
            require(!error, "RTSP second PLAY write failed");
            require(take_rtsp_response(socket, response, yield).starts_with("RTSP/1.0 200"), "playing RTSP session rejected PLAY");
            publish(audio_frame(20'000'001));
            late_output = read_until_closed(socket, response, yield);
        }
        else if (scenario == "play_buffered" || scenario == "play_combined" || scenario == "play_cross_worker")
        {
            // 同一 handler 内同步写入后立即投递失败帧：请求已进入服务端 socket，但读完成尚未被 reactor 收集。
            auto requests = play_request(4);
            if (scenario == "play_combined")
            {
                requests += play_request(5);
            }
            boost::asio::write(socket, boost::asio::buffer(requests));
            publish(audio_frame(20'000'001));
            late_output = read_until_closed(socket, response, yield);
        }
        else
        {
            require(scenario == "play_after_failure", "unknown RTSP play scenario");
            publish(audio_frame(20'000'001));
            boost::asio::async_write(socket, boost::asio::buffer(play_request(4)), yield[error]);
            late_output = read_until_closed(socket, response, yield);
        }
        const auto late_reply = late_output.find("RTSP/1.0") != std::string::npos;
        if (cross_worker)
        {
            // 跨 worker 时失败帧经 drain 投递，读完成可能先于或晚于 drain，两种顺序都必须安全。
            require(!late_reply || late_output.starts_with("RTSP/1.0 200"), "cross worker late PLAY got unexpected reply");
            std::cout << "RTSP play_cross_worker late PLAY " << (late_reply ? "replied before failure" : "dropped after shutdown") << '\n';
        }
        else
        {
            require(!late_reply, "late RTSP PLAY was answered after media failure");
        }
        require(rtsp_mux.live == 0, "failed RTSP play kept its muxer");
        require(stream_registry::instance().find(stream_id) == source, "RTSP play failure removed the source generation");
    };
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
                    if (scenario == "source_ended")
                    {
                        stream_registry::instance().remove(*source);
                        source->end();
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
                        if (accepted && peer.audio_packets != 0)
                        {
                            require(result == 0, "accepted RTMP response failed");
                            require(rtmp_client_getstate(client.get()) == 4, "RTMP config arrived before client Play.Start");
                            break;
                        }
                        if (result != 0)
                        {
                            require(!accepted, "accepted RTMP client failed");
                            break;
                        }
                    }
                    require((peer.audio_packets != 0) == accepted, "RTMP media admission differs from authorization");
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
                    if (scenario == "accepted" || scenario.starts_with("play_"))
                    {
                        require(!error && headers.starts_with("RTSP/1.0 200"), "accepted RTSP DESCRIBE failed");
                    }
                    else if (scenario == "source_ended")
                    {
                        require(!error && headers.starts_with("RTSP/1.0 404"), "RTSP accepted ended source during verification");
                    }
                    else
                    {
                        require(!headers.starts_with("RTSP/1.0 200") &&
                                    (!error || error == boost::asio::error::eof || error == boost::asio::error::connection_reset),
                                "failed RTSP admission emitted a successful SDP");
                    }
                    if (scenario.starts_with("play_"))
                    {
                        play_after_describe(client_socket, response, yield);
                    }
                }
                if (scenario == "late_frame")
                {
                    late_frame_source = source;
                    late_frame_completion = std::make_shared<boost::asio::steady_timer>(worker.io());
                    late_frame_completion->expires_at(std::chrono::steady_clock::time_point::max());
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
                require(successful_play_starts == (accepted ? 1U : 0U), "failed RTMP preparation sent Play.Start");
                require(sent_audio_packets == (accepted ? 1U : 0U), "failed RTMP admission sent codec config");
            }
            if (scenario == "late_frame")
            {
                boost::system::error_code error;
                late_frame_completion->async_wait(yield[error]);
                require(error == boost::asio::error::operation_aborted && !late_frame_source, "late RTMP frame was not exercised");
                late_frame_completion.reset();
            }
            finish();
        },
        completed);
    if (cross_worker)
    {
        source_thread = std::thread([&source_worker]() { source_worker.run(); });
    }
    worker.io().run_for(6s);
    if (!finished)
    {
        finish();
        worker.io().restart();
        worker.io().run_for(1s);
    }
    if (cross_worker)
    {
        source_worker.request_stop();
        source_thread.join();
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    require(observed.live == 0, "play mux remained after shutdown");
    require(invalid_context_sends == 0, "late media used a destroyed RTMP context");
    require(observed.destroyed == (accepted || scenario == "source_ended" || scenario == "no_tracks" ? 1U : 0U), "play mux cleanup count changed");
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
extern "C" void __real_rtmp_server_destroy(rtmp_server_t* server);

extern "C" void __wrap_rtmp_server_destroy(rtmp_server_t* server)
{
    __real_rtmp_server_destroy(server);
    if (auto source = std::exchange(late_frame_source, {}))
    {
        boost::asio::post(source->worker().io(),
                          [source, completion = late_frame_completion]()
                          {
                              source->publish({.track = 1,
                                               .pts_ns = 20'000'000,
                                               .payload = std::make_shared<const std::vector<std::uint8_t>>(
                                                   std::initializer_list<std::uint8_t>{0xf8, 0xff, 0xfe})});
                              completion->cancel();
                          });
    }
}

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
    if (server == nullptr)
    {
        ++invalid_context_sends;
        return -1;
    }
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
            admission(false, "source_ended");
            admission(false, "no_tracks");
            for (const auto* scenario : {"play_normal", "play_replied", "play_buffered", "play_combined", "play_after_failure", "play_cross_worker"})
            {
                admission(false, scenario);
            }
        }
        else
        {
            admission(true, "late_frame");
            player_callback_release();
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "protocol play auth: " << error.what() << '\n';
        return 1;
    }
}
