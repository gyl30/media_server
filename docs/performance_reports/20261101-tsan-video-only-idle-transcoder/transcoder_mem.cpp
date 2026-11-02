#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <vector>

#include <jemalloc/jemalloc.h>
#include <opus/opus.h>

#include "media/codec/audio_transcoder.h"

using namespace media_server;

static std::size_t allocated()
{
    std::uint64_t epoch = 1;
    std::size_t size = sizeof(epoch);
    mallctl("epoch", &epoch, &size, &epoch, size);
    std::size_t value = 0;
    size = sizeof(value);
    mallctl("stats.allocated", &value, &size, nullptr, 0);
    return value;
}

static std::size_t resident()
{
    std::uint64_t epoch = 1;
    std::size_t size = sizeof(epoch);
    mallctl("epoch", &epoch, &size, &epoch, size);
    std::size_t value = 0;
    size = sizeof(value);
    mallctl("stats.resident", &value, &size, nullptr, 0);
    return value;
}

int main(int argc, char** argv)
{
    const int count = argc > 1 ? std::atoi(argv[1]) : 256;
    const int channels = argc > 2 ? std::atoi(argv[2]) : 2;
    audio_transcoder_config config{
        .input = {.codec = codec_id::opus, .sample_rate = 48000, .channel_count = static_cast<std::uint16_t>(channels)},
        .output = {.codec = codec_id::aac, .sample_rate = 48000, .channel_count = static_cast<std::uint16_t>(channels)},
        .input_codec_config = {},
        .output_bit_rate = 64000 * channels,
        .output_cutoff = 20000,
    };
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error);
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<opus_int16> pcm(960 * channels);
    for (int frame = 0; frame < 100; ++frame)
    {
        for (int i = 0; i < 960; ++i)
        {
            for (int c = 0; c < channels; ++c)
            {
                pcm[i * channels + c] = static_cast<opus_int16>(8000 * __builtin_sin((frame * 960 + i) * 2 * 3.14159 * (440 + 220 * c) / 48000));
            }
        }
        std::vector<std::uint8_t> packet(1500);
        const auto bytes = opus_encode(encoder, pcm.data(), 960, packet.data(), static_cast<opus_int32>(packet.size()));
        packet.resize(bytes);
        packets.push_back(std::move(packet));
    }
    opus_encoder_destroy(encoder);

    const auto base_allocated = allocated();
    const auto base_resident = resident();
    std::vector<std::unique_ptr<audio_transcoder>> transcoders;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i)
    {
        transcoders.push_back(std::make_unique<audio_transcoder>());
        if (!transcoders.back()->startup(config))
        {
            std::fprintf(stderr, "startup failed\n");
            return 1;
        }
    }
    const auto startup_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / count;
    const auto after_startup_allocated = allocated();
    const auto after_startup_resident = resident();

    std::vector<media_frame> output;
    const auto transcode_started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 100; ++frame)
    {
        for (auto& transcoder : transcoders)
        {
            media_frame input{.track = 1,
                              .dts_ns = frame * 20'000'000LL,
                              .pts_ns = frame * 20'000'000LL,
                              .key_frame = true,
                              .payload = std::make_shared<const std::vector<std::uint8_t>>(packets[frame])};
            output.clear();
            if (!transcoder->transcode(input, output))
            {
                std::fprintf(stderr, "transcode failed\n");
                return 1;
            }
        }
    }
    const auto transcode_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - transcode_started).count() / count / 100;
    const auto after_transcode_allocated = allocated();
    const auto after_transcode_resident = resident();

    const auto restart_started = std::chrono::steady_clock::now();
    for (auto& transcoder : transcoders)
    {
        transcoder->startup(config);
    }
    const auto restart_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - restart_started).count() / count;

    transcoders.clear();
    const auto after_free_allocated = allocated();
    const auto after_free_resident = resident();
    std::printf("count=%d channels=%d startup_us=%.1f restart_us=%.1f transcode_us_per_frame=%.1f\n", count, channels, startup_us, restart_us, transcode_us);
    std::printf("allocated_per_transcoder_kib startup=%.1f after_100_frames=%.1f\n",
                (after_startup_allocated - base_allocated) / 1024.0 / count, (after_transcode_allocated - base_allocated) / 1024.0 / count);
    std::printf("resident_per_transcoder_kib startup=%.1f after_100_frames=%.1f after_free_total_kib=%.1f allocated_after_free_kib=%.1f\n",
                (after_startup_resident - base_resident) / 1024.0 / count, (after_transcode_resident - base_resident) / 1024.0 / count,
                (static_cast<double>(after_free_resident) - base_resident) / 1024.0, (static_cast<double>(after_free_allocated) - base_allocated) / 1024.0);
    return 0;
}
