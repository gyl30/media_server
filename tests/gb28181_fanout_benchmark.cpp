#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#include <boost/asio/post.hpp>
#include <boost/json.hpp>

#include "media/core/media_stream.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/net/worker_context.h"

extern "C"
{
#include "rtp-packet.h"
}

using namespace media_server;

int main(int argc, char** argv)
{
    const auto viewers = argc > 1 ? std::stoi(argv[1]) : 100;
    const auto seconds = argc > 2 ? std::stoi(argv[2]) : 10;
    const auto frame_bytes = argc > 3 ? std::stoi(argv[3]) : 16'000;
    if (viewers < 1 || seconds < 1 || frame_bytes < 100)
    {
        return 2;
    }
    constexpr std::size_t worker_count = 5;
    std::array<worker_context, worker_count> workers;
    std::vector<std::thread> threads;
    for (auto& worker : workers)
    {
        threads.emplace_back([&worker]() { worker.run(); });
    }
    const auto on_worker = [](worker_context& worker, auto function)
    {
        std::promise<void> completed;
        auto future = completed.get_future();
        boost::asio::post(worker.io(),
                          [function = std::move(function), &completed]() mutable
                          {
                              function();
                              completed.set_value();
                          });
        future.get();
    };
    const std::vector<std::uint8_t> config{
        0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6e, 0x40, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
    };
    auto source = std::make_shared<media_stream>("benchmark/ps", workers[0]);
    on_worker(workers[0],
              [&]()
              {
                  if (!source->set_tracks({
                          {.id = 1, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = config},
                          {.id = 2, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}},
                      }))
                  {
                      std::terminate();
                  }
              });
    struct counters
    {
        std::uint64_t bytes{};
        std::uint64_t packets{};
        std::uint64_t video{};
        std::uint64_t audio{};
        bool video_packet{};
        std::uint16_t next_sequence{};
        bool started{};
        std::uint64_t sequence_errors{};
    };
    std::vector<counters> counts(static_cast<std::size_t>(viewers));
    std::vector<std::shared_ptr<gb28181_rtp_sender>> senders;
    std::atomic_int failures{};
    for (int i = 0; i < viewers; ++i)
    {
        auto& worker = workers[1 + static_cast<std::size_t>(i) % (worker_count - 1)];
        const auto ssrc = static_cast<std::uint32_t>(i + 1);
        auto sender = std::make_shared<gb28181_rtp_sender>(
            worker,
            source,
            96,
            ssrc,
            [&, i, ssrc](std::vector<std::uint8_t> packet)
            {
                rtp_packet_t decoded{};
                if (rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(packet.size())) != 0 || decoded.rtp.ssrc != ssrc)
                {
                    ++failures;
                    return;
                }
                auto& count = counts[static_cast<std::size_t>(i)];
                if (count.started && count.next_sequence != decoded.rtp.seq)
                {
                    ++count.sequence_errors;
                }
                count.started = true;
                count.next_sequence = static_cast<std::uint16_t>(decoded.rtp.seq + 1);
                const auto* data = static_cast<const std::uint8_t*>(decoded.payload);
                if (decoded.payloadlen >= 14 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0xba)
                {
                    for (int pos = 14; pos + 4 < std::min(decoded.payloadlen, 256); ++pos)
                    {
                        if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)
                        {
                            const auto sid = data[pos + 3];
                            if (sid == 0xe0 || sid == 0xc0 || sid == 0xbd)
                            {
                                count.video_packet = sid == 0xe0;
                                break;
                            }
                        }
                    }
                }
                count.bytes += packet.size();
                ++count.packets;
                count.video_packet ? ++count.video : ++count.audio;
            },
            []() {},
            [&]() { ++failures; });
        on_worker(worker,
                  [&]()
                  {
                      if (!sender->startup())
                      {
                          ++failures;
                      }
                  });
        senders.push_back(std::move(sender));
    }
    auto key = config;
    key.insert(key.end(), {0, 0, 0, 1, 0x65});
    key.resize(static_cast<std::size_t>(frame_bytes), 0x55);
    auto delta = std::vector<std::uint8_t>{0, 0, 0, 1, 0x41};
    delta.resize(static_cast<std::size_t>(frame_bytes), 0x55);
    const auto key_payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(key));
    const auto delta_payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(delta));
    const auto audio_payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    const auto cpu_start = std::clock();
    for (int tick = 0; tick < seconds * 50; ++tick)
    {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(tick * 20));
        on_worker(workers[0],
                  [&]()
                  {
                      const auto timestamp = static_cast<std::int64_t>(tick) * 20'000'000;
                      if (tick % 2 == 0)
                      {
                          const bool key_frame = tick % 50 == 0;
                          source->publish({.track = 1,
                                           .dts_ns = timestamp,
                                           .pts_ns = timestamp,
                                           .key_frame = key_frame,
                                           .payload = key_frame ? key_payload : delta_payload});
                      }
                      source->publish({.track = 2, .dts_ns = timestamp, .pts_ns = timestamp, .key_frame = false, .payload = audio_payload});
                  });
    }
    std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto cpu_seconds = static_cast<double>(std::clock() - cpu_start) / CLOCKS_PER_SEC;
    for (std::size_t i = 0; i < senders.size(); ++i)
    {
        on_worker(workers[1 + i % (worker_count - 1)], [&]() { senders[i]->shutdown(); });
    }
    on_worker(workers[0], [&]() { source->end(); });
    for (auto& worker : workers)
    {
        worker.request_stop();
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    std::vector<double> throughput;
    std::uint64_t bytes = 0, packets = 0, sequence_errors = 0;
    auto video_min = counts.front().video;
    auto audio_min = counts.front().audio;
    for (const auto& count : counts)
    {
        throughput.push_back(static_cast<double>(count.bytes) / elapsed);
        bytes += count.bytes;
        packets += count.packets;
        sequence_errors += count.sequence_errors;
        video_min = std::min(video_min, count.video);
        audio_min = std::min(audio_min, count.audio);
    }
    std::ranges::sort(throughput);
    boost::json::object result{
        {"sessions", viewers},
        {"seconds", elapsed},
        {"frame_bytes", frame_bytes},
        {"cpu_cores", cpu_seconds / elapsed},
        {"gbps", static_cast<double>(bytes) * 8.0 / elapsed / 1e9},
        {"pps", static_cast<double>(packets) / elapsed},
        {"bytes_per_viewer_min", throughput.front()},
        {"bytes_per_viewer_p10", throughput[throughput.size() / 10]},
        {"bytes_per_viewer_p50", throughput[throughput.size() / 2]},
        {"bytes_per_viewer_p90", throughput[throughput.size() * 9 / 10]},
        {"bytes_per_viewer_max", throughput.back()},
        {"video_packets_min", video_min},
        {"audio_packets_min", audio_min},
        {"sequence_errors", sequence_errors},
        {"failures", failures.load()},
    };
    std::cout << result << '\n';
    return failures.load() == 0 && sequence_errors == 0 && audio_min > 0 && video_min > 0 ? 0 : 1;
}
