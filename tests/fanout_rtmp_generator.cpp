#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "tests/clients/rtmp_test_client.h"

namespace
{

struct endpoint
{
    std::string host;
    std::string port;
};

struct configuration
{
    endpoint signaling;
    std::string stream_name;
    std::string media_host;
    std::uint16_t media_port{};
    std::size_t viewers{};
    std::chrono::seconds duration{};
    std::size_t ramp_per_second{};
};

struct allocation_result
{
    boost::system::error_code error;
    std::string stream_id;
    unsigned status{};
};

struct results
{
    std::size_t allocated{};
    std::size_t connected{};
    std::size_t ready{};
    std::size_t progressing{};
    std::size_t completed{};
    std::size_t failures{};
    std::size_t pre_measurement_failures{};
    std::size_t runtime_failures{};
    std::size_t non_progressing_viewers{};
    std::map<std::string, std::size_t> failure_phases;
    std::vector<std::uint64_t> first_media_milliseconds;
    std::uint64_t received_video_bytes{};
    std::uint64_t received_video_messages{};
    std::uint64_t steady_video_bytes{};
    std::uint64_t steady_video_messages{};
    std::uint64_t measurement_start_unix_ns{};
    std::uint64_t measurement_end_unix_ns{};
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
    std::chrono::steady_clock::time_point measurement_deadline{};
    std::vector<std::shared_ptr<boost::asio::steady_timer>> ready_gates;
};

std::pair<std::string, std::string> split_rtmp_stream_name(std::string_view stream_name)
{
    const auto slash = stream_name.find('/');
    if (slash == std::string_view::npos || slash == 0U || slash + 1U == stream_name.size())
    {
        throw std::runtime_error("stream-name must contain app and stream path");
    }
    return {std::string(stream_name.substr(0, slash)), std::string(stream_name.substr(slash + 1U))};
}

endpoint parse_endpoint(std::string_view value)
{
    constexpr std::string_view prefix = "http://";
    if (!value.starts_with(prefix))
    {
        throw std::runtime_error("signaling URL must use http://");
    }
    value.remove_prefix(prefix.size());
    const auto slash = value.find('/');
    value = value.substr(0, slash);
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos)
    {
        return {std::string(value), "80"};
    }
    return {std::string(value.substr(0, colon)), std::string(value.substr(colon + 1U))};
}

std::string argument_value(int& index, int argc, char** argv, std::string_view name)
{
    if (index + 1 >= argc || argv[index] != name)
    {
        throw std::runtime_error("missing argument " + std::string(name));
    }
    return argv[++index];
}

configuration parse_arguments(int argc, char** argv)
{
    configuration config{{}, "", "127.0.0.1", 1935, 0, std::chrono::seconds{10}, 50};
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--signaling-url")
        {
            config.signaling = parse_endpoint(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--stream-name")
        {
            config.stream_name = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-host")
        {
            config.media_host = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-port")
        {
            config.media_port = static_cast<std::uint16_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--viewers")
        {
            config.viewers = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--duration")
        {
            config.duration = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
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
    if (config.signaling.host.empty() || config.stream_name.empty() || config.viewers == 0U || config.duration.count() <= 0 ||
        config.ramp_per_second == 0U)
    {
        throw std::runtime_error("signaling-url, stream-name, viewers, duration and ramp-per-second are required");
    }
    return config;
}

boost::asio::awaitable<allocation_result> allocate(const run_state& state)
{
    namespace http = boost::beast::http;
    boost::asio::ip::tcp::resolver resolver(co_await boost::asio::this_coro::executor);
    boost::beast::tcp_stream stream(co_await boost::asio::this_coro::executor);
    boost::system::error_code error;
    const auto endpoints = co_await resolver.async_resolve(
        state.config.signaling.host, state.config.signaling.port, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return allocation_result{error, {}, 0U};
    }
    stream.expires_after(std::chrono::seconds{5});
    co_await stream.async_connect(endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return allocation_result{error, {}, 0U};
    }

    boost::json::object body;
    body["protocol"] = "rtmp";
    body["stream_name"] = state.config.stream_name;
    http::request<http::string_body> request{http::verb::post, "/api/play/allocations", 11};
    request.set(http::field::host, state.config.signaling.host);
    request.set(http::field::content_type, "application/json");
    request.body() = boost::json::serialize(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return allocation_result{error, {}, 0U};
    }

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error)
    {
        co_return allocation_result{error, {}, 0U};
    }
    if (response.result() != http::status::created)
    {
        co_return allocation_result{boost::asio::error::operation_aborted, {}, response.result_int()};
    }
    try
    {
        const auto value = boost::json::parse(response.body()).as_object();
        const auto stream_id = value.at("stream_id").as_string();
        co_return allocation_result{{}, std::string(stream_id.c_str(), stream_id.size()), response.result_int()};
    }
    catch (const std::exception&)
    {
        co_return allocation_result{boost::asio::error::operation_aborted, {}, response.result_int()};
    }
}

void record_failure(run_state& state, std::string_view phase)
{
    ++state.result.failures;
    ++state.result.failure_phases[std::string(phase)];
}

std::uint64_t unix_now_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void release_ready_gates(run_state& state)
{
    for (const auto& gate : state.ready_gates)
    {
        gate->cancel();
    }
}

void abort_before_measurement(run_state& state)
{
    if (state.measurement_started || state.pre_measurement_abort)
    {
        return;
    }
    state.pre_measurement_abort = true;
    ++state.result.pre_measurement_failures;
    release_ready_gates(state);
}

void mark_ready(run_state& state)
{
    ++state.ready_count;
    ++state.result.ready;
    if (state.ready_count == state.config.viewers)
    {
        state.measurement_started = true;
        state.measurement_deadline = std::chrono::steady_clock::now() + state.config.duration;
        state.result.measurement_start_unix_ns = unix_now_ns();
        state.result.measurement_end_unix_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch() + state.config.duration)
                .count());
        release_ready_gates(state);
    }
}

boost::asio::awaitable<void> run_viewer(std::shared_ptr<run_state> state, std::size_t index)
{
    const auto started = std::chrono::steady_clock::now();
    const auto allocation = co_await allocate(*state);
    if (allocation.error)
    {
        record_failure(*state, allocation.status == 0U ? "allocation" : "allocation_http");
        abort_before_measurement(*state);
    }
    else
    {
        ++state->result.allocated;
        const auto stream_parts = split_rtmp_stream_name(state->config.stream_name);
        const auto& app = stream_parts.first;
        const auto& stream = stream_parts.second;
        media_server::test::rtmp_test_client client(state->io, app, stream + "?stream_id=" + allocation.stream_id);
        const auto play_error = co_await client.play(state->config.media_host, state->config.media_port);
        if (play_error)
        {
            record_failure(*state, "connect_or_handshake");
            abort_before_measurement(*state);
        }
        else
        {
            ++state->result.connected;
            state->result.first_media_milliseconds.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
            const auto gate = std::make_shared<boost::asio::steady_timer>(state->io);
            gate->expires_at(std::chrono::steady_clock::time_point::max());
            state->ready_gates.push_back(gate);
            mark_ready(*state);
            boost::system::error_code gate_error;
            if (!state->measurement_started && !state->pre_measurement_abort)
            {
                co_await gate->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, gate_error));
            }
            if (state->pre_measurement_abort)
            {
                record_failure(*state, "pre_measurement_abort");
            }
            else
            {
                const auto bytes_before = client.received_bytes();
                const auto messages_before = client.received_messages();
                const auto consume_error = co_await client.consume_until(state->measurement_deadline);
                if (consume_error)
                {
                    record_failure(*state, "runtime");
                    ++state->result.runtime_failures;
                }
                else if (client.received_bytes() == bytes_before || client.received_messages() == messages_before)
                {
                    record_failure(*state, "non_progressing_media");
                    ++state->result.non_progressing_viewers;
                }
                else
                {
                    ++state->result.progressing;
                    state->result.steady_video_bytes += client.received_bytes() - bytes_before;
                    state->result.steady_video_messages += client.received_messages() - messages_before;
                }
            }
            state->result.received_video_bytes += client.received_bytes();
            state->result.received_video_messages += client.received_messages();
        }
    }
    ++state->result.completed;
    if (state->result.completed == state->config.viewers && state->spawned == state->config.viewers)
    {
        state->io.stop();
    }
    (void)index;
    co_return;
}

boost::asio::awaitable<void> spawn_viewers(std::shared_ptr<run_state> state)
{
    for (std::size_t index = 0; index < state->config.viewers; ++index)
    {
        ++state->spawned;
        boost::asio::co_spawn(state->io, run_viewer(state, index), boost::asio::detached);
        if (index + 1U < state->config.viewers)
        {
            boost::asio::steady_timer timer(state->io);
            timer.expires_after(std::chrono::milliseconds{1000U / state->config.ramp_per_second});
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
    if (state->result.completed == state->config.viewers)
    {
        state->io.stop();
    }
    co_return;
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction)
{
    if (values.empty())
    {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}

void print_results(const run_state& state)
{
    const auto& result = state.result;
    std::cout << "viewers=" << state.config.viewers << " allocated=" << result.allocated << " connected=" << result.connected
              << " ready=" << result.ready << " progressing=" << result.progressing << " failed=" << result.failures
              << " pre_measurement_failures=" << result.pre_measurement_failures
              << " runtime_failures=" << result.runtime_failures
              << " non_progressing_viewers=" << result.non_progressing_viewers
              << " received_video_bytes=" << result.received_video_bytes
              << " received_video_messages=" << result.received_video_messages
              << " steady_video_bytes=" << result.steady_video_bytes
              << " steady_video_messages=" << result.steady_video_messages
              << " measurement_start_unix_ns=" << result.measurement_start_unix_ns
              << " measurement_end_unix_ns=" << result.measurement_end_unix_ns << '\n';
    std::cout << "first_media_ms_p50=" << percentile(result.first_media_milliseconds, 0.50)
              << " p95=" << percentile(result.first_media_milliseconds, 0.95)
              << " p99=" << percentile(result.first_media_milliseconds, 0.99)
              << " max=" << percentile(result.first_media_milliseconds, 1.0) << '\n';
    for (const auto& [phase, count] : result.failure_phases)
    {
        std::cout << "failure_phase=" << phase << " count=" << count << '\n';
    }
}

}    // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto config = parse_arguments(argc, argv);
        boost::asio::io_context io;
        auto state = std::make_shared<run_state>(run_state{io, config, {}, 0U, 0U, false, false, {}, {}});
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
