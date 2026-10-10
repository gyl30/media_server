#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/beast/http.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/codec/codec_utils.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/hls/hls_play_session.h"
#include "media/http/hls_http_session.h"
#include "media/net/worker_context.h"

namespace
{
using namespace media_server;

// 16x16 H.264 SPS/PPS/IDR，由 FFmpeg libx264 生成。
constexpr std::array<std::uint8_t, 45> h264_idr{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc4, 0x89, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};
const std::vector<std::uint8_t> aac_config{0x11, 0x90};

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

media_frame make_frame(track_id track, std::int64_t pts_ns, bool key_frame, std::vector<std::uint8_t> payload)
{
    return media_frame{.track = track,
                       .dts_ns = pts_ns,
                       .pts_ns = pts_ns,
                       .key_frame = key_frame,
                       .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload))};
}

std::shared_ptr<hls_segmenter> start(worker_context& worker, std::vector<media_track> tracks)
{
    auto stream = std::make_shared<media_stream>("hls/test", worker);
    require(stream->set_tracks(std::move(tracks)), "tracks rejected");
    auto segmenter = std::make_shared<hls_segmenter>();
    return segmenter->startup(stream) ? segmenter : nullptr;
}

void video_with_opus(worker_context& worker)
{
    const auto segmenter = start(worker,
                                 {{.id = 1, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = {}},
                                  {.id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}}});
    require(segmenter != nullptr, "video with opus: opus track disabled whole hls");
    for (std::int64_t index = 0; index < 100; ++index)
    {
        const auto pts = index * 40'000'000;
        segmenter->on_frame(make_frame(1, pts, true, {h264_idr.begin(), h264_idr.end()}));
        segmenter->on_frame(make_frame(2, pts, false, std::vector<std::uint8_t>(40, 0x11)));
    }
    require(segmenter->has_segments(), "video with opus: no segments");
    segmenter->shutdown();
    std::cout << "video_with_opus: PASS\n";
}

void audio_only(worker_context& worker)
{
    const auto segmenter = start(worker, {{.id = 2, .kind = media_kind::audio, .codec = codec_id::aac, .clock_rate = 48'000,
                                           .channel_count = 2, .codec_config = aac_config}});
    require(segmenter != nullptr, "audio only: startup failed");
    const auto frame = make_adts_frame(aac_config, std::vector<std::uint8_t>(32, 0x00));
    require(!frame.empty(), "audio only: adts fixture failed");
    for (std::int64_t index = 0; index < 250; ++index)
    {
        segmenter->on_frame(make_frame(2, index * 21'333'333, false, frame));
    }
    require(segmenter->has_segments(), "audio only: never segmented");
    segmenter->shutdown();
    std::cout << "audio_only: PASS\n";
}

void opus_only(worker_context& worker)
{
    auto stream = std::make_shared<media_stream>("hls/test", worker);
    require(stream->set_tracks({{.id = 2, .kind = media_kind::audio, .codec = codec_id::opus, .clock_rate = 48'000, .channel_count = 2, .codec_config = {}}}),
            "tracks rejected");
    const auto segmenter = std::make_shared<hls_segmenter>();
    require(!segmenter->startup(stream), "opus only: unsupported stream accepted");
    // 启动失败后由调用方 shutdown 清理，此时 muxer 尚未创建。
    segmenter->shutdown();
    std::cout << "opus_only: PASS\n";
}

void denied_hls_resources(worker_context& worker)
{
    using tcp = boost::asio::ip::tcp;
    namespace http = boost::beast::http;
    const auto loopback = boost::asio::ip::address_v4::loopback();
    tcp::acceptor signaling(worker.io(), {loopback, 0});
    config application_config;
    application_config.signaling_url = *ada::parse<ada::url_aggregator>(
        "http://127.0.0.1:" + std::to_string(signaling.local_endpoint().port()));
    worker.spawn([&](boost::asio::yield_context yield)
                 {
                     auto socket = signaling.async_accept(yield);
                     boost::beast::flat_buffer buffer;
                     http::request<http::string_body> request;
                     http::async_read(socket, buffer, request, yield);
                     http::response<http::empty_body> response(http::status::forbidden, 11);
                     response.content_length(0);
                     http::async_write(socket, response, yield);
                 });

    auto source = std::make_shared<media_stream>("00000000-0000-0000-0000-000000000001", worker);
    require(source->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                .clock_rate = 90'000, .codec_config = {}}}), "tracks rejected");
    require(stream_registry::instance().add(source), "source registration failed");
    const std::weak_ptr<media_stream> lifetime = source;
    boost::scope::scope_exit cleanup([&]()
                                    {
                                        hls::shutdown();
                                        if (source)
                                        {
                                            stream_registry::instance().remove(*source);
                                            source->end();
                                        }
                                        worker.io().poll();
                                    });

    tcp::acceptor ingress(worker.io(), {loopback, 0});
    boost::beast::tcp_stream client(worker.io());
    client.connect(ingress.local_endpoint());
    http::request<http::string_body> request(http::verb::get,
        "/play/hls/" + source->stream_id() + "/" + std::string(64, 'a') + "/index.m3u8", 11);
    std::make_shared<hls_http_session>(worker, boost::beast::tcp_stream(ingress.accept()),
                                     std::move(request), application_config)->startup();

    http::response<http::string_body> response;
    boost::system::error_code read_error;
    bool completed = false;
    client.expires_after(std::chrono::seconds(3));
    worker.spawn([&](boost::asio::yield_context yield)
                 {
                     boost::beast::flat_buffer buffer;
                     http::async_read(client, buffer, response, yield[read_error]);
                     completed = true;
                 });
    while (!completed)
    {
        worker.io().run_one();
    }
    require(!read_error && response.result() == http::status::forbidden, "invalid HLS token accepted");
    require(response.find(http::field::location) == response.end(), "denied HLS request created a viewer URL");
    client.socket().close();
    stream_registry::instance().remove(*source);
    source->end();
    source.reset();
    worker.io().poll();
    require(lifetime.expired(), "denied HLS request retained the source through a segmenter or viewer");
    std::cout << "denied HLS request creates no segmenter or viewer: PASS\n";
}

void playlist_wait(std::string_view scenario)
{
    using tcp = boost::asio::ip::tcp;
    namespace http = boost::beast::http;
    worker_context worker;
    auto source = std::make_shared<media_stream>("hls/wait", worker);
    require(source->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                .clock_rate = 90'000, .codec_config = {}}}), "wait test tracks rejected");
    auto segmenter = std::make_shared<hls_segmenter>();
    require(segmenter->startup(source), "wait test segmenter failed");
    auto viewer = hls_play_session::create(worker, segmenter);
    const auto loopback = boost::asio::ip::address_v4::loopback();
    tcp::acceptor ingress(worker.io(), {loopback, 0});
    boost::beast::tcp_stream client(worker.io());
    client.connect(ingress.local_endpoint());
    config application_config;
    http::request<http::string_body> request(http::verb::get,
        "/play/hls/session/" + viewer->secret() + "/index.m3u8", 11);
    auto session = std::make_shared<hls_http_session>(worker, boost::beast::tcp_stream(ingress.accept()),
                                                     std::move(request), application_config);
    const std::weak_ptr<hls_http_session> lifetime = session;
    session->startup();
    session.reset();
    boost::asio::steady_timer trigger(worker.io());
    if (scenario != "timeout")
    {
        trigger.expires_after(std::chrono::milliseconds(150));
        trigger.async_wait([&](boost::system::error_code error)
                           {
                               if (error)
                               {
                                   return;
                               }
                               if (scenario == "cancel")
                               {
                                   worker.request_stop();
                                   return;
                               }
                               source->publish(make_frame(1, 0, true, {h264_idr.begin(), h264_idr.end()}));
                               source->publish(make_frame(1, 2'500'000'000, true, {h264_idr.begin(), h264_idr.end()}));
                               source->end();
                           });
    }

    http::response<http::string_body> response;
    boost::system::error_code read_error;
    bool completed = false;
    client.expires_after(std::chrono::seconds(12));
    const auto started = std::chrono::steady_clock::now();
    worker.spawn([&](boost::asio::yield_context yield)
                 {
                     boost::beast::flat_buffer buffer;
                     http::async_read(client, buffer, response, yield[read_error]);
                     completed = true;
                     worker.request_stop();
                 });
    worker.io().run_for(std::chrono::seconds(13));
    require(completed, "playlist request exceeded deadline");
    if (scenario == "cancel")
    {
        require(static_cast<bool>(read_error), "cancelled playlist request completed successfully");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "playlist shutdown waited for timeout");
    }
    else if (scenario == "timeout")
    {
        require(!read_error && response.result() == http::status::service_unavailable, "empty playlist did not time out with 503");
        require(std::chrono::steady_clock::now() - started >= std::chrono::seconds(10), "playlist deadline became shorter");
    }
    else
    {
        require(!read_error && response.result() == http::status::ok, "waiting playlist did not become ready");
        require(response.body().starts_with("#EXTM3U") && response.body().contains("#EXT-X-ENDLIST"),
                "playlist lost delayed frames or source end");
    }
    require(lifetime.expired(), "playlist wait retained HTTP session after shutdown");
    segmenter->shutdown();
    std::cout << "playlist_wait " << scenario << ": PASS\n";
}
}    // namespace

int main()
{
    try
    {
        worker_context worker;
        video_with_opus(worker);
        audio_only(worker);
        opus_only(worker);
        denied_hls_resources(worker);
        for (const auto scenario : {"ready", "timeout", "cancel"})
        {
            playlist_wait(scenario);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
