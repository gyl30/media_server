#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/codec/codec_utils.h"
#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/rtsp/rtsp_play_session.h"
#include "media/webrtc/whep_audio_egress.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace
{
using namespace media_server;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void timestamp_boundaries()
{
    constexpr std::int64_t cycle = std::int64_t{1} << 32;
    rtmp_timestamp_state state;
    require(unwrap_rtmp_timestamp(0xfffffff0U, state) == cycle - 16, "initial RTMP timestamp changed");
    require(unwrap_rtmp_timestamp(10, state) == cycle + 10, "RTMP wrap broke the timeline");
    require(unwrap_rtmp_timestamp(10, state) == cycle + 10, "duplicate RTMP timestamp advanced");
    require(unwrap_rtmp_timestamp(8, state) == cycle + 8, "small RTMP timestamp correction became a wrap");
    require(unwrap_rtmp_timestamp(42, state) == cycle + 42, "RTMP timeline did not continue after wrap");
    require(rtmp_timestamp_delta(0, 0xffffffffU) == 1, "RTMP forward wrap delta incorrect");
    require(rtmp_timestamp_delta(0xffffffffU, 0) == -1, "RTMP reverse wrap delta incorrect");
    rtmp_timestamp_state replacement;
    require(unwrap_rtmp_timestamp(0, replacement) == 0, "new generation retained an RTMP timeline");
    require(milliseconds_to_ns(cycle + 27) == 4'294'967'323'000'000, "unwrapped RTMP nanoseconds truncated");
    require(ns_to_flv_milliseconds(4'294'967'323'000'000) == 27, "FLV wire timestamp did not wrap");
    require(ns_to_milliseconds(1'234'567) == 1, "millisecond truncation changed");
    require(ns_to_milliseconds(-1'234'567) == -1, "negative millisecond truncation changed");
    require(ns_to_90khz(1'234'567) == 111, "sub-millisecond precision lost in 90 kHz conversion");
    require(ns_to_90khz(-1'234'567) == -111, "negative 90 kHz truncation changed");
    require(ns_to_90khz(999'999'999) == 89'999, "90 kHz second boundary rounded up");
    require(ns_to_90khz(1'000'000'000) == 90'000, "90 kHz second boundary incorrect");
    require(ns_to_90khz(2'592'000'123'456'789) == 233'280'011'111, "long-running 90 kHz conversion overflowed");
    std::cout << "RTMP wrap/replacement and ms/90 kHz timestamp boundaries: PASS\n";
}

template <typename Function>
auto on(worker_context& worker, Function function)
{
    using result = std::invoke_result_t<Function>;
    std::promise<result> promise;
    auto future = promise.get_future();
    boost::asio::post(worker.io(), [function = std::move(function), promise = std::move(promise)]() mutable
                      {
                          try
                          {
                              if constexpr (std::is_void_v<result>)
                              {
                                  function();
                                  promise.set_value();
                              }
                              else
                              {
                                  promise.set_value(function());
                              }
                          }
                          catch (...)
                          {
                              promise.set_exception(std::current_exception());
                          }
                      });
    require(future.wait_for(5s) == std::future_status::ready, "worker operation did not complete");
    return future.get();
}

struct workers
{
    worker_context source;
    worker_context viewer;
    std::thread source_thread{[this]() { source.run(); }};
    std::thread viewer_thread{[this]() { viewer.run(); }};

    ~workers()
    {
        source.request_stop();
        viewer.request_stop();
        source_thread.join();
        viewer_thread.join();
    }
};

struct recording_sink final : media_sink
{
    worker_context& owner;
    std::vector<media_frame> frames;
    std::promise<std::vector<media_frame>> ended;
    bool closed{};

    explicit recording_sink(worker_context& worker) : owner(worker) {}
    worker_context& worker() noexcept override { return owner; }
    void on_frame(const media_frame& frame) override
    {
        if (!closed)
        {
            frames.push_back(frame);
        }
    }
    void on_end() override { ended.set_value(std::move(frames)); }
};

void rtsp_sender_clock(workers& context)
{
    std::string reply;
    rtsp_handler_t handler{};
    handler.close = [](void*) { return 0; };
    handler.send = [](void* param, const void* data, std::size_t bytes)
    {
        static_cast<std::string*>(param)->assign(static_cast<const char*>(data), bytes);
        return 0;
    };
    std::unique_ptr<rtsp_server_t, decltype(&rtsp_server_destroy)> server(
        rtsp_server_create("127.0.0.1", 8554, &handler, nullptr, &reply), &rtsp_server_destroy);
    require(server != nullptr, "RTSP server creation failed");
    auto source = std::make_shared<media_stream>("verify/sender-clock", context.source);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reports;
    auto player = std::make_shared<rtsp_play_session>(context.source,
        source->stream_id(), boost::asio::ip::make_address("127.0.0.1"),
        [&](std::vector<std::uint8_t> packet)
        {
            if (packet[1] == 1)
            {
                require(packet.size() >= 32 && packet[5] == 200, "missing RTCP sender report");
                reports.emplace_back(boost::endian::load_big_u64(packet.data() + 12),
                                     boost::endian::load_big_u32(packet.data() + 20));
            }
        });
    boost::scope::scope_exit cleanup([&]()
    {
        on(context.source, [&]()
        {
            player->shutdown();
            stream_registry::instance().remove(*source);
            source->end();
        });
        on(context.source, []() {});
    });
    on(context.source, [&]()
    {
        require(source->set_tracks({{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a,
                                     .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}}), "RTSP clock tracks failed");
        require(stream_registry::instance().add(source), "RTSP clock registration failed");
        rtsp_header_transport_t transport{};
        require(rtsp_header_transport("RTP/AVP/TCP;unicast;interleaved=0-1", &transport) == 0, "RTSP transport parse failed");
        require(player->on_setup(server.get(), "rtsp://127.0.0.1/verify/sender-clock/trackID=1", {}, &transport, 1) == 0,
                "RTSP clock SETUP failed");
        const auto begin = reply.find("Session: ");
        require(begin != std::string::npos, "RTSP SETUP omitted session");
        const auto end = reply.find_first_of(";\r\n", begin + 9);
        const auto session = reply.substr(begin + 9, end - begin - 9);
        require(player->on_play(server.get(), "rtsp://127.0.0.1/verify/sender-clock", session, nullptr, nullptr) == 0,
                "RTSP clock PLAY failed");
    });
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5);
    for (int frame = 0; frame < 3; ++frame)
    {
        if (frame != 0)
        {
            std::this_thread::sleep_for(4s);
        }
        on(context.source, [&]()
        {
            const auto pts = milliseconds_to_ns(frame * 20);
            source->publish({.track = 1, .dts_ns = pts, .pts_ns = pts, .payload = payload});
        });
    }
    require(reports.size() == 3, "RTSP clock test did not span three report intervals");
    for (const auto& [ntp, rtp] : reports)
    {
        const auto ntp_ticks = static_cast<long double>(ntp - reports.front().first) * 8'000 / (std::uint64_t{1} << 32);
        const auto rtp_ticks = static_cast<std::uint32_t>(rtp - reports.front().second);
        require(std::abs(ntp_ticks - rtp_ticks) < 2, "RTCP sender clock followed packet arrival instead of media timeline");
    }
    std::cout << "RTSP sender reports retain clock mapping across delayed media bursts: PASS\n";
}

void ordered_generations_and_churn(workers& context)
{
    auto& registry = stream_registry::instance();
    std::shared_ptr<media_stream> previous;
    const auto payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5);
    for (int generation = 0; generation < 20; ++generation)
    {
        auto source = on(context.source, [&]()
                         {
                             auto stream = std::make_shared<media_stream>("verify/generation", context.source);
                             require(stream->set_tracks({{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a,
                                                          .clock_rate = 8000, .channel_count = 1, .codec_config = {}}}), "initial tracks rejected");
                             require(registry.add(stream), "generation not registered");
                             if (previous)
                             {
                                 registry.remove(*previous);
                                 require(registry.find(stream->stream_id()) == stream, "old cleanup removed new generation");
                             }
                             return stream;
                         });
        previous.reset();
        auto sink = std::make_shared<recording_sink>(context.viewer);
        auto ended = sink->ended.get_future();
        on(context.source, [&]()
           {
               source->add_sink(sink);
               for (int frame = 0; frame < 20; ++frame)
               {
                   const auto timestamp = static_cast<std::int64_t>(generation) * 1'000'000'000 + frame * 20'000'000;
                   source->publish({.track = 1, .dts_ns = timestamp, .pts_ns = timestamp, .payload = payload});
               }
               registry.remove(*source);
               source->end();
           });
        require(ended.wait_for(5s) == std::future_status::ready, "old sink did not end");
        const auto frames = ended.get();
        require(frames.size() == 20, "source end overtook pending frames");
        for (int frame = 0; frame < 20; ++frame)
        {
            const auto expected = static_cast<std::int64_t>(generation) * 1'000'000'000 + frame * 20'000'000;
            require(frames[static_cast<std::size_t>(frame)].pts_ns == expected, "cross-worker frame order/generation mismatch");
        }
        previous = std::move(source);
    }
    const std::weak_ptr<media_stream> old = previous;
    previous.reset();
    require(old.expired(), "ended source retained after last owner left");
    require(!registry.find("verify/generation"), "ended generation remains registered");

    auto source = on(context.source, [&]()
                     {
                         auto stream = std::make_shared<media_stream>("verify/churn", context.source);
                         require(stream->set_tracks({{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a,
                                                      .clock_rate = 8000, .channel_count = 1, .codec_config = {}}}), "churn tracks rejected");
                         return stream;
                     });
    for (int iteration = 0; iteration < 300; ++iteration)
    {
        auto sink = std::make_shared<recording_sink>(context.viewer);
        on(context.source, [&]()
           {
               source->add_sink(sink);
               source->publish({.track = 1, .pts_ns = 20'000'000, .payload = payload});
           });
        on(context.viewer, [&]()
           {
               require(sink->frames.size() == 1, "new subscription did not receive media");
               sink->closed = true;
           });
        on(context.source, [&]() { source->remove_sink(sink.get()); });
        const std::weak_ptr<recording_sink> weak = sink;
        sink.reset();
        on(context.viewer, []() {});
        require(weak.expired(), "removed sink retained");
    }
    on(context.source, [&]() { source->end(); });
    std::cout << "cross-worker generations=20, ordered frames=400, sink churn=300: PASS\n";
}

void derived_generation_lifetimes(workers& context)
{
    on(context.source, [&]() { hls::startup(context.source); });
    std::vector<std::weak_ptr<media_stream>> sources;
    std::shared_ptr<hls_segmenter> previous_hls;
    std::shared_ptr<mpeg_ps_output> previous_ps;
    std::shared_ptr<whep_audio_egress> previous_audio;
    for (int generation = 0; generation < 20; ++generation)
    {
        on(context.source, [&]()
           {
               auto source = std::make_shared<media_stream>("verify/derived", context.source);
               sources.emplace_back(source);
               require(source->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                            .clock_rate = 90000, .codec_config = {}},
                                           {.id = 2, .kind = media_kind::audio, .codec = codec_id::aac,
                                            .clock_rate = 48000, .channel_count = 2, .codec_config = {0x11, 0x90}}}), "derived tracks rejected");
               require(stream_registry::instance().add(source), "derived generation not registered");
               auto ps = source->ps_output();
               require(ps && source->ps_output() == ps && ps != previous_ps, "PS shared across wrong generation");
               auto audio = acquire_whep_audio_egress(source, context.source, {.channels = 2, .bitrate = 64000, .max_playback_rate = 48000});
               auto second = acquire_whep_audio_egress(source, context.source, {.channels = 2, .bitrate = 64000, .max_playback_rate = 48000});
               require(audio && audio == second && audio != previous_audio, "AAC egress shared across wrong generation");
               require(audio->output_stream()->tracks().at(1).codec == codec_id::opus, "derived audio is not Opus");
               auto segmenter = hls::get_or_create(source->stream_id());
               require(segmenter && segmenter != previous_hls && !segmenter->ended_at(), "HLS shared across wrong generation");
               if (previous_hls)
               {
                   require(previous_hls->ended_at().has_value(), "old HLS segmenter remained live");
               }
               stream_registry::instance().remove(*source);
               source->end();
               previous_hls = std::move(segmenter);
               previous_ps = std::move(ps);
               previous_audio = audio;
               release_whep_audio_egress(second);
               release_whep_audio_egress(audio);
           });
    }
    on(context.source, [&]()
       {
           hls::shutdown();
           previous_hls.reset();
           previous_ps.reset();
           previous_audio.reset();
       });
    for (const auto& source : sources)
    {
        require(source.expired(), "derived output retained ended source");
    }
    std::cout << "HLS/PS/shared AAC->Opus source generations=20 and source release: PASS\n";
}
struct registered_session final : session
{
    unsigned shutdown_count{};
    void shutdown() override { ++shutdown_count; }
};

void session_registry_identity()
{
    auto& registry = session_registry::instance();
    const std::string stream_id = "verify/session-identity";
    auto previous = std::make_shared<registered_session>();
    auto current = std::make_shared<registered_session>();
    auto sender = std::make_shared<registered_session>();
    auto other_sender = std::make_shared<registered_session>();
    require(registry.add_receiver_session(stream_id, previous), "receiver registration failed");
    require(!registry.add_receiver_session(stream_id, current), "duplicate receiver accepted");
    require(!registry.take_receiver_session("missing"), "missing receiver returned a session");
    require(registry.find_receiver_session(stream_id) == previous, "wrong ID removed receiver");
    require(registry.receivers() == std::vector<std::string>{stream_id}, "receiver list did not contain only stream ID");
    require(registry.take_receiver_session(stream_id) == previous, "receiver take failed");
    require(!registry.take_receiver_session(stream_id), "receiver take was not destructive");
    require(registry.add_receiver_session(stream_id, current), "replacement receiver registration failed");
    registry.remove_receiver_session(stream_id, *previous);
    require(registry.find_receiver_session(stream_id) == current, "old receiver cleanup removed replacement");
    require(registry.add_sender_session(stream_id, "sender", previous), "sender registration failed");
    require(!registry.add_sender_session(stream_id, "sender", sender), "duplicate sender accepted");
    require(registry.add_sender_session(stream_id, "other", other_sender), "distinct sender rejected");
    require(registry.take_sender_session(stream_id, "sender") == previous, "sender take failed");
    require(registry.add_sender_session(stream_id, "sender", sender), "replacement sender registration failed");
    registry.remove_sender_session(stream_id, "sender", *previous);
    require(!registry.take_sender_session("missing", "sender"), "wrong stream ID removed sender");
    require(!registry.take_sender_session(stream_id, "missing"), "wrong sender ID removed sender");
    registry.remove_receiver_session(stream_id, *current);
    require(registry.receivers().empty(), "sender was listed as receiver");
    require(registry.take_sender_session(stream_id, "sender") == sender, "old sender cleanup removed replacement");
    require(registry.take_sender_session(stream_id, "other") == other_sender, "receiver removal removed sender");
    require(!registry.take_sender_session(stream_id, "sender"), "sender take was not destructive");
    std::cout << "session registry stream ID, sender IDs and object-identity cleanup: PASS\n";
}

void session_registry_stop()
{
    auto& registry = session_registry::instance();
    auto receiver = std::make_shared<registered_session>();
    auto sender = std::make_shared<registered_session>();
    require(registry.add_receiver_session("verify/session-stop", receiver), "stop receiver registration failed");
    require(registry.add_sender_session("verify/session-stop", "sender", sender), "stop sender registration failed");
    registry.shutdown_all();
    require(receiver->shutdown_count == 1 && sender->shutdown_count == 1, "registry stop did not close all sessions once");
    require(registry.receivers().empty(), "registry stop retained receiver");
    require(!registry.take_sender_session("verify/session-stop", "sender"), "registry stop retained sender");
    require(!registry.add_receiver_session("verify/late", receiver), "stopped registry accepted receiver");
    require(!registry.add_sender_session("verify/late", "sender", sender), "stopped registry accepted sender");
    registry.shutdown_all();
    require(receiver->shutdown_count == 1 && sender->shutdown_count == 1, "repeat registry stop closed sessions twice");
    std::cout << "session registry terminal stop with empty resources: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        timestamp_boundaries();
        workers context;
        rtsp_sender_clock(context);
        ordered_generations_and_churn(context);
        derived_generation_lifetimes(context);
        session_registry_identity();
        session_registry_stop();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
