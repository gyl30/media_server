#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>

#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls.h"
#include "media/hls/hls_segmenter.h"
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"
#include "media/webrtc/whep_audio_egress.h"

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
                                 require(registry.find(stream->name()) == stream, "old cleanup removed new generation");
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
               auto segmenter = hls::get_or_create(source->name());
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
}    // namespace

int main()
{
    try
    {
        workers context;
        ordered_generations_and_churn(context);
        derived_generation_lifetimes(context);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
