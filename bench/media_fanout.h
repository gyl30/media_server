#ifndef MEDIA_SERVER_BENCH_MEDIA_FANOUT_H
#define MEDIA_SERVER_BENCH_MEDIA_FANOUT_H

#include <map>
#include <ctime>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <iostream>
#include <algorithm>
#include <string_view>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "bench/media_progress.h"
#include "bench/credential_file.h"
#include "bench/measurement_start.h"

namespace media_server::bench
{

template <typename Client>
class media_fanout final
{
   private:
    struct configuration
    {
        std::string stream_name;
        std::string play_paths_file;
        std::string start_file;
        std::vector<std::string> play_paths;
        std::string media_host{"127.0.0.1"};
        std::uint16_t media_port{};
        std::size_t viewers{};
        std::chrono::seconds warmup{5};
        std::chrono::seconds duration{10};
        std::size_t ramp_per_second{50};
        std::chrono::seconds ready_timeout{60};
        bool video_only{};
        std::chrono::seconds progress_interval{5};
    };

    struct results
    {
        std::size_t connected{};
        std::size_t ready{};
        std::size_t progressing{};
        std::size_t completed{};
        std::size_t failures{};
        std::size_t pre_measurement_failures{};
        std::size_t trigger_failures{};
        std::size_t pre_measurement_abort_viewers{};
        std::size_t collateral_aborted_viewers{};
        std::size_t runtime_failures{};
        std::size_t non_progressing_viewers{};
        std::map<std::string, std::size_t> failure_phases;
        std::map<std::string, std::size_t> runtime_errors;
        std::vector<std::uint64_t> first_media_milliseconds;
        std::uint64_t received_video_bytes{};
        std::uint64_t received_video_messages{};
        std::uint64_t steady_video_bytes{};
        std::uint64_t steady_video_messages{};
        std::uint64_t steady_audio_bytes{};
        std::uint64_t steady_audio_messages{};
        std::vector<std::uint64_t> viewer_video_bytes;
        std::vector<std::uint64_t> viewer_audio_bytes;
        std::vector<std::uint64_t> viewer_video_media_ms;
        std::uint64_t measurement_start_unix_ns{};
        std::uint64_t measurement_end_unix_ns{};
        std::uint64_t measurement_start_monotonic_ns{};
        std::uint64_t measurement_end_monotonic_ns{};
        std::uint64_t trigger_unix_ns{};
        std::clock_t measurement_cpu_start{};
        std::clock_t measurement_cpu_end{};
        std::string trigger_failure_phase;
        std::string trigger_error;
    };

    struct run_state
    {
        boost::asio::io_context& io;
        configuration config;
        results result;
        std::size_t spawned{};
        std::size_t ready_count{};
        bool measurement_started{};
        bool pre_measurement_abort{};
        std::size_t trigger_viewer = static_cast<std::size_t>(-1);
        std::chrono::steady_clock::time_point measurement_deadline{};
        std::vector<std::shared_ptr<boost::asio::steady_timer>> ready_gates;
        bool measurement_finished{};
        std::vector<std::shared_ptr<Client>> clients{};
        std::vector<media_server::bench::media_counters> media_before{};
        std::vector<media_server::bench::media_counters> media_after{};
    };

    static std::string argument_value(int& index, int argc, char** argv, std::string_view name)
    {
        if (index + 1 >= argc || argv[index] != name)
        {
            throw std::runtime_error("missing argument " + std::string(name));
        }
        return argv[++index];
    }

    static configuration parse_arguments(int argc, char** argv, std::uint16_t default_port)
    {
        configuration config;
        config.media_port = default_port;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument = argv[index];
            if (argument == "--stream-name")
            {
                config.stream_name = argument_value(index, argc, argv, argument);
            }
            else if (argument == "--play-paths-file")
            {
                config.play_paths_file = argument_value(index, argc, argv, argument);
            }
            else if (argument == "--start-file")
            {
                config.start_file = argument_value(index, argc, argv, argument);
            }
            else if (argument == "--media-host")
            {
                config.media_host = argument_value(index, argc, argv, argument);
            }
            else if (argument == "--media-port")
            {
                const auto port = std::stoul(argument_value(index, argc, argv, argument));
                if (port == 0U || port > 65535U)
                {
                    throw std::runtime_error("media-port must be in 1..65535");
                }
                config.media_port = static_cast<std::uint16_t>(port);
            }
            else if (argument == "--video-only")
            {
                config.video_only = true;
            }
            else if (argument == "--progress-interval")
            {
                config.progress_interval = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
            }
            else if (argument == "--ready-timeout")
            {
                config.ready_timeout = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
            }
            else if (argument == "--viewers")
            {
                config.viewers = std::stoull(argument_value(index, argc, argv, argument));
            }
            else if (argument == "--duration")
            {
                config.duration = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
            }
            else if (argument == "--warmup")
            {
                config.warmup = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
            }
            else if (argument == "--ramp-per-second")
            {
                config.ramp_per_second = std::stoull(argument_value(index, argc, argv, argument));
            }
            else
            {
                throw std::runtime_error("unknown argument " + std::string(argument));
            }
        }
        if (config.stream_name.empty() == config.play_paths_file.empty() || config.viewers == 0U || config.duration.count() <= 0 ||
            config.warmup.count() < 0 || config.ramp_per_second == 0U || config.ready_timeout.count() <= 0 || config.progress_interval.count() <= 0)
        {
            throw std::runtime_error("exactly one of stream-name or play-paths-file, viewers, positive duration and ramp-per-second are required");
        }
        if (!config.play_paths_file.empty())
        {
            config.play_paths = media_server::bench::read_credentials(config.play_paths_file, config.viewers);
        }
        return config;
    }

    static void record_failure(run_state& state, std::string_view phase, std::size_t viewer)
    {
        ++state.result.failures;
        ++state.result.failure_phases[std::string(phase)];
        if (state.result.trigger_failure_phase.empty())
        {
            state.result.trigger_failure_phase = phase;
            state.result.trigger_unix_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            state.trigger_viewer = viewer;
        }
    }

    static void record_runtime_failure(run_state& state, const boost::system::error_code& error)
    {
        ++state.result.runtime_failures;
        if (state.result.trigger_error.empty())
        {
            state.result.trigger_error = std::to_string(error.value()) + ":" + error.message();
        }
        ++state.result.runtime_errors[std::to_string(error.value()) + ":" + error.message()];
    }

    static std::string error_text(const boost::system::error_code& error) { return std::to_string(error.value()) + ":" + error.message(); }

    static std::uint64_t unix_now_ns()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static void abort_ready_viewers(run_state& state)
    {
        for (const auto& gate : state.ready_gates)
        {
            gate->expires_at(std::chrono::steady_clock::now());
        }
    }

    static void abort_before_measurement(run_state& state, std::size_t viewer, std::string_view phase, std::string error)
    {
        if (state.measurement_started || state.pre_measurement_abort)
        {
            return;
        }
        state.pre_measurement_abort = true;
        ++state.result.pre_measurement_failures;
        ++state.result.trigger_failures;
        state.result.trigger_failure_phase = std::string(phase);
        state.result.trigger_error = std::move(error);
        state.result.trigger_unix_ns = unix_now_ns();
        state.trigger_viewer = viewer;
        abort_ready_viewers(state);
    }

    static void mark_ready(run_state& state)
    {
        ++state.ready_count;
        ++state.result.ready;
        if (!state.pre_measurement_abort && state.ready_count == state.config.viewers)
        {
            std::cout << "phase=media_ready ready=" << state.ready_count << std::endl;
            boost::asio::co_spawn(
                state.io,
                media_server::bench::wait_measurement_start(state.io, state.config.start_file, state.config.warmup, state.config.ready_timeout),
                [&state](std::exception_ptr error, std::chrono::steady_clock::time_point start)
                {
                    if (error)
                    {
                        try
                        {
                            std::rethrow_exception(error);
                        }
                        catch (const std::exception& failure)
                        {
                            record_failure(state, "measurement_barrier", 0);
                            abort_before_measurement(state, 0, "measurement_barrier", failure.what());
                        }
                        return;
                    }
                    if (state.pre_measurement_abort)
                    {
                        return;
                    }
                    for (std::size_t index = 0; index < state.clients.size(); ++index)
                    {
                        state.media_before[index] = media_server::bench::sample_media(*state.clients[index]);
                    }
                    state.measurement_started = true;
                    state.measurement_deadline = start + state.config.duration;
                    state.result.measurement_start_monotonic_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    state.result.measurement_start_unix_ns = unix_now_ns();
                    state.result.measurement_cpu_start = std::clock();
                    auto deadline = std::make_shared<boost::asio::steady_timer>(state.io);
                    deadline->expires_at(state.measurement_deadline);
                    deadline->async_wait(
                        [&state, deadline](const boost::system::error_code& deadline_error)
                        {
                            if (!deadline_error)
                            {
                                for (std::size_t index = 0; index < state.clients.size(); ++index)
                                {
                                    state.media_after[index] = media_server::bench::sample_media(*state.clients[index]);
                                }
                                state.measurement_finished = true;
                                state.result.measurement_cpu_end = std::clock();
                                state.result.measurement_end_monotonic_ns = static_cast<std::uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
                                state.result.measurement_end_unix_ns = unix_now_ns();
                                abort_ready_viewers(state);
                                if (state.result.completed == state.config.viewers)
                                {
                                    state.io.stop();
                                }
                            }
                        });
                    state.result.measurement_end_unix_ns =
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch() + state.config.duration)
                                                       .count());
                    std::cout << "phase=measurement_start unix_ns=" << state.result.measurement_start_unix_ns
                              << " monotonic_ns=" << state.result.measurement_start_monotonic_ns << std::endl;
                });
        }
    }

    static boost::asio::awaitable<void> run_viewer(std::shared_ptr<run_state> state, std::size_t index)
    {
        const auto started = std::chrono::steady_clock::now();
        auto client =
            std::make_shared<Client>(state->io, (state->config.play_paths.empty() ? state->config.stream_name : state->config.play_paths[index]));
        state->clients[index] = client;
        auto timed_out = std::make_shared<bool>(false);
        boost::asio::steady_timer ready_timer(state->io);
        ready_timer.expires_after(state->config.ready_timeout);
        ready_timer.async_wait(
            [client, timed_out](const boost::system::error_code& error)
            {
                if (!error)
                {
                    *timed_out = true;
                    client->cancel();
                }
            });
        auto play_error = co_await client->play(state->config.media_host, state->config.media_port, !state->config.video_only);
        ready_timer.cancel();
        if (*timed_out)
        {
            play_error = boost::asio::error::timed_out;
        }
        if (play_error)
        {
            const auto phase = play_error == boost::system::errc::permission_denied ? "authorization"
                               : client->handshake_complete() ? (play_error == boost::asio::error::timed_out ? "first_media_timeout" : "first_media")
                                                              : "connect_or_handshake";
            record_failure(*state, phase, index);
            abort_before_measurement(*state, index, phase, error_text(play_error));
        }
        else
        {
            ++state->result.connected;
            state->result.first_media_milliseconds.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
            const auto gate = std::make_shared<boost::asio::steady_timer>(state->io);
            gate->expires_at(std::chrono::steady_clock::time_point::max());
            gate->async_wait([client](const boost::system::error_code&) { client->cancel(); });
            state->ready_gates.push_back(gate);
            mark_ready(*state);

            const auto& before = state->media_before[index];
            bool measurement_snapshot_taken = false;
            auto progress_timer = std::make_shared<boost::asio::steady_timer>(state->io);
            auto progress = std::make_shared<media_server::bench::media_progress>();
            boost::system::error_code consume_error;
            while (!state->pre_measurement_abort)
            {
                if (!measurement_snapshot_taken && state->measurement_started)
                {
                    measurement_snapshot_taken = true;
                    boost::asio::co_spawn(state->io,
                                          media_server::bench::watch_media(client,
                                                                           progress_timer,
                                                                           progress,
                                                                           !state->config.video_only,
                                                                           state->config.progress_interval,
                                                                           state->measurement_deadline),
                                          boost::asio::detached);
                }
                if (measurement_snapshot_taken && std::chrono::steady_clock::now() >= state->measurement_deadline)
                {
                    break;
                }
                consume_error = co_await client->consume_one();
                if (consume_error)
                {
                    if ((state->measurement_finished || progress->stalled) && consume_error == boost::asio::error::operation_aborted)
                    {
                        consume_error.clear();
                    }
                    break;
                }
            }
            const auto after = state->measurement_finished ? state->media_after[index] : media_server::bench::sample_media(*client);
            if (consume_error && !measurement_snapshot_taken)
            {
                record_failure(*state, "consume_before_measurement", index);
                abort_before_measurement(*state, index, "consume_before_measurement", error_text(consume_error));
            }
            if (measurement_snapshot_taken && state->measurement_finished &&
                std::chrono::steady_clock::now() - progress->sampled >= std::chrono::seconds{1} &&
                media_server::bench::media_stalled(*client, *progress, !state->config.video_only))
            {
                progress->stalled = true;
            }
            progress_timer->cancel();
            gate->cancel();
            if (state->pre_measurement_abort && index != state->trigger_viewer)
            {
                record_failure(*state, "pre_measurement_abort", index);
                ++state->result.pre_measurement_abort_viewers;
                ++state->result.collateral_aborted_viewers;
            }
            else
            {
                if (consume_error)
                {
                    record_failure(*state, "runtime", index);
                    record_runtime_failure(*state, consume_error);
                }
                else if (!measurement_snapshot_taken || progress->stalled || after.video_bytes == before.video_bytes ||
                         after.video_messages == before.video_messages ||
                         (!state->config.video_only && after.audio_messages == before.audio_messages))
                {
                    record_failure(*state, "non_progressing_media", index);
                    ++state->result.non_progressing_viewers;
                }
                else
                {
                    ++state->result.progressing;
                    const auto video_bytes = after.video_bytes - before.video_bytes;
                    const auto audio_bytes = after.audio_bytes - before.audio_bytes;
                    state->result.steady_video_bytes += video_bytes;
                    state->result.steady_video_messages += after.video_messages - before.video_messages;
                    state->result.steady_audio_bytes += audio_bytes;
                    state->result.steady_audio_messages += after.audio_messages - before.audio_messages;
                    state->result.viewer_video_bytes[index] = video_bytes;
                    state->result.viewer_audio_bytes[index] = audio_bytes;
                    state->result.viewer_video_media_ms.push_back(after.video_media_ms - before.video_media_ms);
                }
            }
            state->result.received_video_bytes += client->received_bytes();
            state->result.received_video_messages += client->received_messages();
        }
        ++state->result.completed;
        if (state->result.completed == state->config.viewers && state->spawned == state->config.viewers &&
            (!state->measurement_started || state->measurement_finished))
        {
            state->io.stop();
        }
        (void)index;
        co_return;
    }

    static boost::asio::awaitable<void> spawn_viewers(std::shared_ptr<run_state> state)
    {
        for (std::size_t index = 0; index < state->config.viewers; ++index)
        {
            ++state->spawned;
            boost::asio::co_spawn(state->io, run_viewer(state, index), boost::asio::detached);
            if (index + 1U < state->config.viewers)
            {
                boost::asio::steady_timer timer(state->io);
                timer.expires_after(std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(state->config.ramp_per_second)});
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
        }
        if (state->result.completed == state->config.viewers)
        {
            state->io.stop();
        }
        co_return;
    }

    static std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction)
    {
        if (values.empty())
        {
            return 0;
        }
        std::sort(values.begin(), values.end());
        const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U));
        return values[index];
    }

    static double realtime_ratio(const results& result, double fraction)
    {
        const auto window_ns = result.measurement_end_monotonic_ns - result.measurement_start_monotonic_ns;
        if (result.measurement_end_monotonic_ns <= result.measurement_start_monotonic_ns || result.viewer_video_media_ms.empty())
        {
            return 0;
        }
        return static_cast<double>(percentile(result.viewer_video_media_ms, fraction)) * 1e6 / static_cast<double>(window_ns);
    }

    static void print_results(const run_state& state)
    {
        const auto& result = state.result;
        std::cout << "phase=measurement requested=" << state.config.viewers << " connected=" << result.connected << " ready=" << result.ready
                  << " progressing=" << result.progressing << " failed=" << result.failures
                  << " pre_measurement_failures=" << result.pre_measurement_failures << " trigger_failures=" << result.trigger_failures
                  << " pre_measurement_abort_viewers=" << result.pre_measurement_abort_viewers
                  << " collateral_aborted_viewers=" << result.collateral_aborted_viewers << " runtime_failures=" << result.runtime_failures
                  << " non_progressing_viewers=" << result.non_progressing_viewers << " received_video_bytes=" << result.received_video_bytes
                  << " received_video_messages=" << result.received_video_messages << " steady_video_bytes=" << result.steady_video_bytes
                  << " steady_video_messages=" << result.steady_video_messages << " steady_audio_bytes=" << result.steady_audio_bytes
                  << " steady_audio_messages=" << result.steady_audio_messages
                  << " viewer_video_bytes_min=" << percentile(result.viewer_video_bytes, 0.0)
                  << " viewer_video_bytes_p10=" << percentile(result.viewer_video_bytes, 0.1)
                  << " viewer_video_bytes_p50=" << percentile(result.viewer_video_bytes, 0.5)
                  << " viewer_video_bytes_p90=" << percentile(result.viewer_video_bytes, 0.9)
                  << " viewer_video_bytes_max=" << percentile(result.viewer_video_bytes, 1.0)
                  << " viewer_audio_bytes_min=" << percentile(result.viewer_audio_bytes, 0.0)
                  << " viewer_audio_bytes_p10=" << percentile(result.viewer_audio_bytes, 0.1)
                  << " viewer_audio_bytes_p50=" << percentile(result.viewer_audio_bytes, 0.5)
                  << " viewer_audio_bytes_p90=" << percentile(result.viewer_audio_bytes, 0.9)
                  << " viewer_audio_bytes_max=" << percentile(result.viewer_audio_bytes, 1.0)
                  << " viewer_realtime_min=" << realtime_ratio(result, 0.0) << " viewer_realtime_p10=" << realtime_ratio(result, 0.1)
                  << " viewer_realtime_p50=" << realtime_ratio(result, 0.5) << " viewer_realtime_max=" << realtime_ratio(result, 1.0)
                  << " measurement_start_unix_ns=" << result.measurement_start_unix_ns
                  << " measurement_start_monotonic_ns=" << result.measurement_start_monotonic_ns
                  << " measurement_end_monotonic_ns=" << result.measurement_end_monotonic_ns
                  << " measurement_end_unix_ns=" << result.measurement_end_unix_ns << " measurement_started=" << (state.measurement_started ? 1 : 0)
                  << " client_cpu_cores="
                  << (state.measurement_started ? static_cast<double>((state.measurement_finished ? result.measurement_cpu_end : std::clock()) -
                                                                      result.measurement_cpu_start) /
                                                      static_cast<double>(CLOCKS_PER_SEC) / static_cast<double>(state.config.duration.count())
                                                : 0.0)
                  << " trigger_viewer=" << state.trigger_viewer << " trigger_unix_ns=" << result.trigger_unix_ns << '\n';
        std::cout << "trigger_failure_phase=" << result.trigger_failure_phase << '\n';
        std::cout << "trigger_error=" << result.trigger_error << '\n';
        std::cout << "first_media_ms_p50=" << percentile(result.first_media_milliseconds, 0.50)
                  << " p95=" << percentile(result.first_media_milliseconds, 0.95) << " p99=" << percentile(result.first_media_milliseconds, 0.99)
                  << " max=" << percentile(result.first_media_milliseconds, 1.0) << '\n';
        for (const auto& [phase, count] : result.failure_phases)
        {
            std::cout << "failure_phase=" << phase << " count=" << count << '\n';
        }
        for (const auto& [error, count] : result.runtime_errors)
        {
            std::cout << "runtime_error=" << error << " count=" << count << '\n';
        }
    }

   public:
    static int run(int argc, char** argv, std::uint16_t default_port)
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << "--play-paths-file FILE | --stream-name PATH\n"
                         "--viewers N --media-host HOST --media-port PORT\n"
                         "--ramp-per-second N --ready-timeout SECONDS\n"
                         "--warmup SECONDS --duration SECONDS --progress-interval SECONDS\n"
                         "--start-file FILE (shared measurement time in CLOCK_MONOTONIC nanoseconds)\n"
                         "--video-only (default requires video and audio)\n";
            return 0;
        }
        try
        {
            const auto config = parse_arguments(argc, argv, default_port);
            boost::asio::io_context io;
            auto state = std::make_shared<run_state>(run_state{io, config, {}, 0U, 0U, false, false, static_cast<std::size_t>(-1), {}, {}});
            state->result.viewer_video_bytes.resize(config.viewers);
            state->result.viewer_audio_bytes.resize(config.viewers);
            state->clients.resize(config.viewers);
            state->media_before.resize(config.viewers);
            state->media_after.resize(config.viewers);
            boost::asio::co_spawn(io, spawn_viewers(state), boost::asio::detached);
            io.run();
            print_results(*state);
            return state->result.ready == state->config.viewers && state->result.progressing == state->config.viewers ? 0 : 1;
        }
        catch (const std::exception& error)
        {
            std::cerr << "fanout generator failed: " << error.what() << '\n';
            return 2;
        }
    }
};

}    // namespace media_server::bench

#endif
