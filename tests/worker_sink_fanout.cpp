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
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"

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
void on_owner(worker_context& worker, Function function)
{
    boost::asio::post(worker.io(), std::move(function));
    require(worker.io().poll() != 0, "owner operation did not run");
}

struct recording
{
    std::vector<std::int64_t> events;
    std::vector<byte_buffer> payloads;
    bool valid_payloads{true};
    std::promise<void>* entered{};
    std::shared_future<void> release;

    void frame(std::int64_t timestamp, byte_buffer payload)
    {
        events.push_back(timestamp / 1'000'000);
        payloads.push_back(std::move(payload));
        if (auto* signal = std::exchange(entered, nullptr))
        {
            signal->set_value();
            release.wait();
        }
    }

    void end() { events.push_back(-1); }
};

struct canonical_sink final : media_sink
{
    worker_context& owner;
    std::shared_ptr<recording> records{std::make_shared<recording>()};

    explicit canonical_sink(worker_context& worker) : owner(worker) {}
    worker_context& worker() noexcept override { return owner; }
    void on_frame(const media_frame& frame) override { records->frame(frame.pts_ns, frame.payload); }
    void on_end() override { records->end(); }
};

struct ps_sink final : mpeg_ps_sink
{
    worker_context& owner;
    std::shared_ptr<recording> records{std::make_shared<recording>()};

    explicit ps_sink(worker_context& worker) : owner(worker) {}
    worker_context& worker() noexcept override { return owner; }
    void on_ps_frame(const mpeg_ps_frame& frame) override
    {
        const auto& payload = frame.payload;
        records->valid_payloads = records->valid_payloads && payload && payload->size() >= 4 &&
                                  (*payload)[0] == 0 && (*payload)[1] == 0 && (*payload)[2] == 1 && (*payload)[3] == 0xba;
        records->frame(frame.pts_ns, payload);
    }
    void on_end() override { records->end(); }
};

template <bool Ps>
struct fixture
{
    using sink_type = std::conditional_t<Ps, ps_sink, canonical_sink>;
    std::shared_ptr<media_stream> source;
    std::shared_ptr<mpeg_ps_output> output;
    byte_buffer payload;

    explicit fixture(worker_context& owner)
    {
        const std::vector<std::uint8_t> config{
            0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6e, 0x40,
            0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
        };
        auto key = config;
        key.insert(key.end(), {0, 0, 0, 1, 0x65});
        key.resize(160, 0x55);
        payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(key));
        on_owner(owner, [&]()
                 {
                     source = std::make_shared<media_stream>("verify/fanout", owner);
                     require(source->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264,
                                                 .clock_rate = 90000, .codec_config = config}}), "fanout tracks rejected");
                     if constexpr (Ps)
                     {
                         output = source->ps_output();
                         require(output != nullptr, "PS output startup failed");
                     }
                 });
    }

    void add(const std::shared_ptr<sink_type>& sink)
    {
        if constexpr (Ps)
        {
            output->add_sink(sink);
        }
        else
        {
            source->add_sink(sink);
        }
    }

    void remove(sink_type* sink)
    {
        if constexpr (Ps)
        {
            output->remove_sink(sink);
        }
        else
        {
            source->remove_sink(sink);
        }
    }

    void publish(std::int64_t sequence)
    {
        const auto timestamp = sequence * 1'000'000;
        source->publish({.track = 1, .dts_ns = timestamp, .pts_ns = timestamp, .key_frame = true, .payload = payload});
    }

    void end() { source->end(); }
};

template <bool Ps>
void inline_delivery_and_validation()
{
    worker_context owner;
    fixture<Ps> live(owner);
    auto sink = std::make_shared<typename fixture<Ps>::sink_type>(owner);
    auto late = std::make_shared<typename fixture<Ps>::sink_type>(owner);
    on_owner(owner, [&]()
             {
                 live.add(sink);
                 live.publish(1);
                 live.publish(2);
                 require(sink->records->events == std::vector<std::int64_t>{1, 2}, "same-worker delivery was deferred");
                 live.source->publish({.track = 2, .payload = live.payload});
                 live.source->publish({.track = 1, .payload = {}});
                 live.source->publish({.track = 1, .payload = std::make_shared<const std::vector<std::uint8_t>>()});
                 require(sink->records->events.size() == 2, "invalid canonical frame was delivered");
                 live.end();
                 live.end();
                 live.publish(3);
                 require(sink->records->events == std::vector<std::int64_t>{1, 2, -1}, "same-worker end/lifecycle changed");
                 live.add(late);
                 require(late->records->events.empty(), "late subscription end must remain posted");
             });
    require(late->records->events == std::vector<std::int64_t>{-1}, "late subscription did not end once");
    require(sink->records->valid_payloads, "invalid PS packet");
    if constexpr (!Ps)
    {
        require(sink->records->payloads[0] == live.payload, "inline canonical payload was copied");
    }
}

template <bool Ps>
void grouped_delivery_and_end()
{
    worker_context owner;
    worker_context target;
    worker_context other;
    worker_context third;
    fixture<Ps> live(owner);
    std::vector<std::shared_ptr<typename fixture<Ps>::sink_type>> sinks;
    for (auto* worker : {&target, &target, &target, &other, &third})
    {
        sinks.push_back(std::make_shared<typename fixture<Ps>::sink_type>(*worker));
        live.add(sinks.back());
    }
    on_owner(owner, [&]()
             {
                 live.publish(1);
                 live.publish(2);
                 live.publish(3);
                 live.end();
             });
    const std::weak_ptr<media_stream> source = live.source;
    const std::weak_ptr<mpeg_ps_output> output = live.output;
    live.output.reset();
    live.source.reset();
    require(source.expired() && output.expired(), "posted drain retained business owner");
    require(sinks[0]->records->events.empty(), "cross-worker callback ran on source owner");
    require(other.io().poll() == 1 && third.io().poll() == 1, "target group scheduling changed");
    require(sinks[0]->records->events.empty(), "one target drain ran another group");
    require(target.io().poll() == 1, "same target sinks required multiple scheduled drains");
    for (const auto& sink : sinks)
    {
        require(sink->records->events == std::vector<std::int64_t>{1, 2, 3, -1}, "frame/end ordering changed");
        require(sink->records->valid_payloads, "invalid PS frame in cross-worker delivery");
    }
    require(sinks[0]->records->payloads == sinks[1]->records->payloads &&
            sinks[1]->records->payloads == sinks[2]->records->payloads, "group fanout copied payload bytes");
}

template <bool Ps>
void expired_and_removed_sinks()
{
    worker_context owner;
    worker_context target;
    fixture<Ps> live(owner);
    auto expired = std::make_shared<typename fixture<Ps>::sink_type>(target);
    const auto expired_records = expired->records;
    const std::weak_ptr<typename fixture<Ps>::sink_type> expired_weak = expired;
    on_owner(owner, [&]() { live.add(expired); });
    expired.reset();
    require(expired_weak.expired(), "registration retained weak sink");
    on_owner(owner, [&]() { live.publish(1); });
    require(target.io().poll() == 1, "expired group did not drain");
    require(expired_records->events.empty(), "expired sink received a callback");

    auto removed = std::make_shared<typename fixture<Ps>::sink_type>(target);
    auto survivor = std::make_shared<typename fixture<Ps>::sink_type>(target);
    const auto removed_records = removed->records;
    const std::weak_ptr<typename fixture<Ps>::sink_type> removed_weak = removed;
    on_owner(owner, [&]()
             {
                 live.add(removed);
                 live.add(survivor);
                 live.publish(2);
                 live.publish(3);
                 live.remove(removed.get());
                 live.end();
             });
    removed.reset();
    require(removed_weak.expired(), "pending group retained removed sink");
    require(target.io().poll() == 1, "pending removal changed group drain scheduling");
    require(removed_records->events.empty(), "remove before snapshot still delivered pending frames/end");
    require(survivor->records->events == std::vector<std::int64_t>{2, 3, -1}, "removal damaged other sink");
}

template <bool Ps>
void remove_after_snapshot()
{
    worker_context owner;
    worker_context target;
    fixture<Ps> live(owner);
    auto blocker = std::make_shared<typename fixture<Ps>::sink_type>(target);
    auto removed = std::make_shared<typename fixture<Ps>::sink_type>(target);
    const auto removed_records = removed->records;
    const std::weak_ptr<typename fixture<Ps>::sink_type> removed_weak = removed;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    blocker->records->entered = &entered;
    blocker->records->release = release.get_future().share();
    on_owner(owner, [&]()
             {
                 live.add(blocker);
                 live.add(removed);
                 live.publish(1);
             });
    std::thread drain([&]() { target.io().poll(); });
    try
    {
        require(entered_future.wait_for(5s) == std::future_status::ready, "snapshot callback did not start");
        on_owner(owner, [&]()
                 {
                     live.remove(removed.get());
                     live.end();
                 });
        removed.reset();
        require(!removed_weak.expired(), "snapshot failed to retain sink during removal");
        require(removed_records->events.empty(), "blocked snapshot callback ran early");
    }
    catch (...)
    {
        release.set_value();
        drain.join();
        throw;
    }
    release.set_value();
    drain.join();
    require(removed_records->events == std::vector<std::int64_t>{1}, "already-snapshotted callback semantics changed");
    require(removed_weak.expired(), "finished snapshot retained removed sink");
    require(blocker->records->events == std::vector<std::int64_t>{1, -1}, "snapshot/end ordering changed");
}

template <bool Ps>
void boundary_and_overflow()
{
    {
        worker_context owner;
        worker_context target;
        fixture<Ps> live(owner);
        auto sink = std::make_shared<typename fixture<Ps>::sink_type>(target);
        on_owner(owner, [&]()
                 {
                     live.add(sink);
                     for (std::int64_t frame = 1; frame <= 2500; ++frame)
                     {
                         live.publish(frame);
                     }
                     live.end();
                 });
        require(target.io().poll() == 1, "boundary queue scheduled more than one drain");
        require(sink->records->events.size() == 2501 && sink->records->events.back() == -1,
                "2500-frame queue overflowed or end overtook pending frames");
        for (std::int64_t frame = 1; frame <= 2500; ++frame)
        {
            require(sink->records->events[static_cast<std::size_t>(frame - 1)] == frame, "boundary queue order changed");
        }
    }
    worker_context owner;
    worker_context slow;
    worker_context fast;
    worker_context other;
    fixture<Ps> live(owner);
    auto slow_sink = std::make_shared<typename fixture<Ps>::sink_type>(slow);
    auto fast_sink = std::make_shared<typename fixture<Ps>::sink_type>(fast);
    auto other_sink = std::make_shared<typename fixture<Ps>::sink_type>(other);
    on_owner(owner, [&]()
             {
                 live.add(slow_sink);
                 live.add(fast_sink);
                 live.add(other_sink);
             });
    for (std::int64_t frame = 1; frame <= 2502; ++frame)
    {
        on_owner(owner, [&]() { live.publish(frame); });
        require(fast.io().poll() == 1 && other.io().poll() == 1, "slow group blocked another target worker");
    }
    require(slow.io().poll() == 1, "overflow changed scheduled drain count");
    require(slow_sink->records->events == std::vector<std::int64_t>{-1}, "overflow did not clear pending and end group");
    on_owner(owner, [&]()
             {
                 live.publish(2503);
                 live.end();
             });
    require(slow.io().poll() == 0, "overflowed group was not detached");
    require(fast.io().poll() == 1 && other.io().poll() == 1, "unaffected group final drain missing");
    require(slow_sink->records->events == std::vector<std::int64_t>{-1}, "overflow group received double end");
    for (const auto& sink : {fast_sink, other_sink})
    {
        require(sink->records->events.size() == 2504 && sink->records->events.back() == -1, "overflow damaged another group");
        for (std::int64_t frame = 1; frame <= 2503; ++frame)
        {
            require(sink->records->events[static_cast<std::size_t>(frame - 1)] == frame, "unaffected group order changed");
        }
        require(sink->records->valid_payloads, "invalid PS payload in overflow regression");
    }
}

template <bool Ps>
void verify()
{
    inline_delivery_and_validation<Ps>();
    grouped_delivery_and_end<Ps>();
    expired_and_removed_sinks<Ps>();
    remove_after_snapshot<Ps>();
    boundary_and_overflow<Ps>();
    std::cout << (Ps ? "MPEG-PS" : "canonical media") << " inline/grouped/weak/remove/end/2500-boundary/overflow: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        verify<false>();
        verify<true>();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
