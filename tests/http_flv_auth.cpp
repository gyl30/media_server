#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <optional>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <ada.h>
#include <boost/json.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/core/media_stream.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/http_flv_session.h"

extern "C"
{
#include "flv-muxer.h"
#include "flv-header.h"
#include "flv-writer.h"
}

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

unsigned writer_attempts{};
unsigned writers_created{};
unsigned writers_destroyed{};
unsigned muxers_created{};
unsigned muxers_destroyed{};
unsigned writer_failures{};
bool fail_writer{};
int writer_audio{};
int writer_video{};

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void admission(std::string_view scenario)
{
    require(writers_created == writers_destroyed && muxers_created == muxers_destroyed && !fail_writer, "previous scenario retained FLV resources");
    const auto initial_writer_attempts = writer_attempts;
    const auto initial_writers_created = writers_created;
    const auto initial_muxers_created = muxers_created;
    const auto initial_writer_failures = writer_failures;

    worker_context worker;
    const auto loopback = boost::asio::ip::address_v4::loopback();
    tcp::acceptor signaling(worker.io(), {loopback, 0});
    tcp::acceptor ingress(worker.io(), {loopback, 0});
    config application_config;
    application_config.signaling_url = *ada::parse<ada::url_aggregator>("http://127.0.0.1:" + std::to_string(signaling.local_endpoint().port()));
    const std::string stream_id(64, 'a');
    const std::string token(64, 'b');
    std::optional<std::string> pending_token{token};
    const auto token_expires_at = std::chrono::steady_clock::now() + (scenario == "expired" ? -1s : 1h);
    unsigned verify_requests{};
    unsigned consumed_tokens{};
    unsigned expected_writer_attempts = writer_attempts;
    unsigned expected_muxers_created = muxers_created;
    std::exception_ptr failure;
    bool completed{};
    const bool pending_output = scenario == "slow_close" || scenario == "pending_end" || scenario == "pending_stop" || scenario == "queue_overflow";

    auto source = std::make_shared<media_stream>(stream_id, worker);
    require(source->set_tracks(
                {{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}}),
            "source tracks rejected");
    require(stream_registry::instance().add(source), "source registration failed");
    const std::weak_ptr<media_stream> source_lifetime = source;
    std::shared_ptr<media_stream> replacement;
    std::weak_ptr<http_flv_session> first_session;
    std::weak_ptr<http_flv_session> second_session;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            boost::system::error_code error;
            signaling.close(error);
            ingress.close(error);
            if (source)
            {
                stream_registry::instance().remove(*source);
                source->end();
                source.reset();
            }
            if (replacement)
            {
                stream_registry::instance().remove(*replacement);
                replacement->end();
                replacement.reset();
            }
            fail_writer = false;
            worker.request_stop();
            worker.io().restart();
            worker.io().poll();
        });
    const auto finished = [&](std::exception_ptr error)
    {
        if (error)
        {
            if (!failure)
            {
                failure = error;
            }
            boost::system::error_code close_error;
            signaling.close(close_error);
            worker.request_stop();
        }
    };

    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            for (;;)
            {
                tcp::socket socket(worker.io());
                boost::system::error_code error;
                signaling.async_accept(socket, yield[error]);
                if (error == boost::asio::error::operation_aborted)
                {
                    return;
                }
                require(!error, "verification accept failed");
                boost::beast::flat_buffer buffer;
                http::request<http::string_body> request;
                http::async_read(socket, buffer, request, yield[error]);
                require(!error && request.target() == "/internal/verify", "verification request failed");
                ++verify_requests;
                const auto body = boost::json::parse(request.body()).as_object();
                require(body.at("token").as_string() == token && body.at("stream_id").as_string() == stream_id &&
                            body.at("operation").as_string() == "play",
                        "verification identity changed");
                require(writer_attempts == expected_writer_attempts && muxers_created == expected_muxers_created,
                        "FLV resources initialized before authorization");
                const bool authorized =
                    scenario != "denied" && pending_token && *pending_token == token && std::chrono::steady_clock::now() < token_expires_at;
                if (authorized)
                {
                    pending_token.reset();
                    ++consumed_tokens;
                    if (scenario == "writer_failure")
                    {
                        fail_writer = true;
                    }
                    if (scenario == "replacement")
                    {
                        stream_registry::instance().remove(*source);
                        source->end();
                        source.reset();
                        replacement = std::make_shared<media_stream>(stream_id, worker);
                        // 既有 FFmpeg 16x16 fixture 的 SPS/PPS，不含视频 AU。
                        constexpr std::array<std::uint8_t, 32> config_bytes{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04,
                                                                            0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4,
                                                                            0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8};
                        require(replacement->set_tracks({{.id = 1,
                                                          .kind = media_kind::video,
                                                          .codec = codec_id::h264,
                                                          .clock_rate = 90'000,
                                                          .codec_config = {config_bytes.begin(), config_bytes.end()}}}) &&
                                    stream_registry::instance().add(replacement),
                                "replacement registration failed");
                    }
                }
                http::response<http::empty_body> response(authorized ? http::status::ok : http::status::forbidden, 11);
                response.content_length(0);
                http::async_write(socket, response, yield[error]);
                require(!error, "verification response failed");
            }
        },
        finished);

    boost::asio::spawn(
        worker.io(),
        [&](boost::asio::yield_context yield)
        {
            const auto attempts = scenario == "accepted" || scenario == "writer_failure" ? 2U : 1U;
            for (unsigned attempt = 0; attempt < attempts; ++attempt)
            {
                boost::beast::tcp_stream client(worker.io());
                client.expires_after(3s);
                client.socket().open(tcp::v4());
                if (pending_output)
                {
                    client.socket().set_option(boost::asio::socket_base::receive_buffer_size(1024));
                }
                client.connect(ingress.local_endpoint());
                std::string path = "/" + stream_id + "/" + token + ".flv";
                if (scenario == "invalid_path")
                {
                    path += "?x=1";
                }
                http::request<http::string_body> request(http::verb::get, path, 11);
                expected_writer_attempts = writer_attempts;
                expected_muxers_created = muxers_created;
                auto socket = ingress.accept();
                if (pending_output)
                {
                    socket.set_option(boost::asio::socket_base::send_buffer_size(1024));
                    boost::asio::socket_base::send_buffer_size send_buffer;
                    socket.get_option(send_buffer);
                    require(send_buffer.value() < 1024 * 1024, "test send buffer cannot force a pending media write");
                }
                auto session =
                    std::make_shared<http_flv_session>(worker, boost::beast::tcp_stream(std::move(socket)), std::move(request), application_config);
                (attempt == 0 ? first_session : second_session) = session;
                session->startup();
                session.reset();
                boost::beast::flat_buffer buffer;
                http::response_parser<http::dynamic_body> parser;
                parser.body_limit(4096);
                boost::system::error_code error;
                http::async_read_header(client, buffer, parser, yield[error]);
                require(!error, "HTTP-FLV response header failed");
                auto expected = http::status::ok;
                if (scenario == "invalid_path")
                {
                    expected = http::status::bad_request;
                }
                if (scenario == "denied" || scenario == "expired" || attempt != 0)
                {
                    expected = http::status::forbidden;
                }
                if (scenario == "writer_failure" && attempt == 0)
                {
                    expected = http::status::internal_server_error;
                }
                require(parser.get().result() == expected, "HTTP-FLV admission status changed");
                if (expected == http::status::ok)
                {
                    require(parser.chunked(), "HTTP-FLV output is not chunked");
                    if (scenario != "replacement")
                    {
                        while (parser.get().body().size() < 13U)
                        {
                            http::async_read_some(client, buffer, parser, yield[error]);
                            require(!error || parser.get().body().size() >= 13U, "HTTP-FLV initial body failed");
                        }
                        const auto body = boost::beast::buffers_to_string(parser.get().body().data());
                        require(body.starts_with("FLV"), "HTTP-FLV initial header missing");
                    }
                    require(writer_audio == 1 && writer_video == 0, "HTTP-FLV used replacement tracks after authorization");
                    if (scenario == "normal_media")
                    {
                        for (std::int64_t index = 0; index < 2; ++index)
                        {
                            source->publish({.track = 1,
                                             .dts_ns = index * 20'000'000,
                                             .pts_ns = index * 20'000'000,
                                             .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0x35 + index)});
                        }
                        while (parser.get().body().size() < 365U)
                        {
                            http::async_read_some(client, buffer, parser, yield[error]);
                            require(!error, "HTTP-FLV media chunks failed");
                        }
                        const auto body = boost::beast::buffers_to_string(parser.get().body().data());
                        const auto* bytes = reinterpret_cast<const std::uint8_t*>(body.data());
                        std::size_t offset = 13;
                        for (unsigned index = 0; index < 2; ++index)
                        {
                            flv_tag_header_t tag{};
                            const auto header_bytes = flv_tag_header_read(&tag, bytes + offset, body.size() - offset);
                            require(header_bytes == 11 && tag.type == 8 && tag.size == 161 && tag.timestamp == index * 20,
                                    "HTTP-FLV tag boundary or timestamp changed");
                            offset += static_cast<std::size_t>(header_bytes);
                            require(std::all_of(
                                        bytes + offset + 1, bytes + offset + tag.size, [index](std::uint8_t value) { return value == 0x35 + index; }),
                                    "HTTP-FLV media payload or ordering changed");
                            offset += tag.size;
                            std::uint32_t previous_size{};
                            require(flv_tag_size_read(bytes + offset, body.size() - offset, &previous_size) == 4 && previous_size == 11 + tag.size,
                                    "HTTP-FLV previous tag size changed");
                            offset += 4;
                        }
                        require(offset == body.size(), "HTTP-FLV unexpected trailing bytes");
                    }
                    if (pending_output)
                    {
                        const auto payload = std::make_shared<const std::vector<std::uint8_t>>(1024U * 1024U, 0x35);
                        const auto frames = scenario == "queue_overflow" ? 6 : 1;
                        for (int index = 0; index < frames; ++index)
                        {
                            source->publish({.track = 1, .dts_ns = index * 20'000'000LL, .pts_ns = index * 20'000'000LL, .payload = payload});
                        }
                        boost::asio::steady_timer wait(worker.io(), 20ms);
                        wait.async_wait(yield);
                        if (scenario != "queue_overflow")
                        {
                            require(!first_session.expired() && client.socket().available(error) < payload->size() && !error,
                                    "slow client did not retain a pending media write");
                        }
                        if (scenario == "pending_end")
                        {
                            source->end();
                        }
                        if (scenario == "pending_stop")
                        {
                            worker.request_stop();
                        }
                        if (scenario == "queue_overflow")
                        {
                            require(first_session.expired(), "HTTP-FLV overflow did not terminate a slow viewer");
                            require(stream_registry::instance().find(stream_id) == source, "slow viewer cleanup removed its media source");
                        }
                    }
                }
                else
                {
                    require(writer_attempts == expected_writer_attempts + (scenario == "writer_failure" && attempt == 0 ? 1U : 0U),
                            "rejected request initialized an FLV writer");
                    if (scenario != "writer_failure" || attempt != 0)
                    {
                        require(muxers_created == expected_muxers_created, "rejected request initialized an FLV muxer");
                    }
                }
                client.socket().close(error);
            }
            if (scenario == "invalid_path")
            {
                require(verify_requests == 0 && consumed_tokens == 0 && pending_token, "invalid path consumed authorization");
            }
            else if (scenario == "denied" || scenario == "expired")
            {
                require(verify_requests == 1 && consumed_tokens == 0 && pending_token, "denied token was consumed");
            }
            else
            {
                require(verify_requests == attempts && consumed_tokens == 1 && !pending_token, "token replay was admitted");
            }
            if (scenario == "replacement")
            {
                require(stream_registry::instance().find(stream_id) == replacement, "old source cleanup removed replacement");
            }
            completed = true;
            boost::system::error_code error;
            signaling.close(error);
            worker.request_stop();
        },
        finished);

    worker.io().run_for(5s);
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    require(completed && worker.io().stopped(), "HTTP-FLV authorization exceeded five-second deadline");
    require(first_session.expired() && second_session.expired(), "HTTP-FLV session retained after socket shutdown");
    require(writers_created == writers_destroyed && muxers_created == muxers_destroyed, "HTTP-FLV resource cleanup unbalanced");
    if (scenario == "invalid_path" || scenario == "denied" || scenario == "expired")
    {
        require(writer_attempts == initial_writer_attempts && muxers_created == initial_muxers_created,
                "rejected HTTP-FLV request allocated resources");
    }
    if (scenario == "writer_failure")
    {
        require(!fail_writer && writer_failures == initial_writer_failures + 1U && writers_created == initial_writers_created,
                "FLV writer failure was not exercised");
    }
    if (source)
    {
        stream_registry::instance().remove(*source);
        source->end();
        source.reset();
    }
    require(source_lifetime.expired(), "HTTP-FLV retained the original source");
    std::cout << "HTTP-FLV " << scenario << ": PASS\n";
}
}    // namespace

extern "C" void* __real_flv_writer_create2(int audio, int video, flv_writer_onwrite write, void* param);
extern "C" void __real_flv_writer_destroy(void* writer);
extern "C" flv_muxer_t* __real_flv_muxer_create(flv_muxer_handler handler, void* param);
extern "C" void __real_flv_muxer_destroy(flv_muxer_t* muxer);

extern "C" void* __wrap_flv_writer_create2(int audio, int video, flv_writer_onwrite write, void* param)
{
    ++writer_attempts;
    writer_audio = audio;
    writer_video = video;
    if (fail_writer)
    {
        fail_writer = false;
        ++writer_failures;
        return nullptr;
    }
    auto* writer = __real_flv_writer_create2(audio, video, write, param);
    if (writer != nullptr)
    {
        ++writers_created;
    }
    return writer;
}

extern "C" void __wrap_flv_writer_destroy(void* writer)
{
    if (writer != nullptr)
    {
        ++writers_destroyed;
    }
    __real_flv_writer_destroy(writer);
}

extern "C" flv_muxer_t* __wrap_flv_muxer_create(flv_muxer_handler handler, void* param)
{
    auto* muxer = __real_flv_muxer_create(handler, param);
    if (muxer != nullptr)
    {
        ++muxers_created;
    }
    return muxer;
}

extern "C" void __wrap_flv_muxer_destroy(flv_muxer_t* muxer)
{
    if (muxer != nullptr)
    {
        ++muxers_destroyed;
    }
    __real_flv_muxer_destroy(muxer);
}

int main(int argc, char** argv)
{
    try
    {
        require(argc <= 2, "unexpected test arguments");
        for (const std::string_view scenario : {"invalid_path",
                                                "denied",
                                                "expired",
                                                "accepted",
                                                "writer_failure",
                                                "replacement",
                                                "normal_media",
                                                "slow_close",
                                                "pending_end",
                                                "pending_stop",
                                                "queue_overflow"})
        {
            if (argc == 1 || scenario == argv[1])
            {
                admission(scenario);
            }
        }
        if (argc == 2)
        {
            require(std::string_view(argv[1]) == "invalid_path" || std::string_view(argv[1]) == "denied" || std::string_view(argv[1]) == "expired" ||
                        std::string_view(argv[1]) == "accepted" || std::string_view(argv[1]) == "writer_failure" ||
                        std::string_view(argv[1]) == "replacement" || std::string_view(argv[1]) == "normal_media" ||
                        std::string_view(argv[1]) == "slow_close" || std::string_view(argv[1]) == "pending_end" ||
                        std::string_view(argv[1]) == "pending_stop" || std::string_view(argv[1]) == "queue_overflow",
                    "unknown scenario");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
