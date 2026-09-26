#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

#include "media/core/media_stream.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/ps/mpeg_ps_output.h"

extern "C"
{
#include "rtp-packet.h"
#include "rtp-profile.h"
#include "rtsp-muxer.h"
}

using namespace media_server;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void drain(worker_context& worker)
{
    worker.io().restart();
    while (worker.io().poll() > 0)
    {
    }
}

class ps_reader final : public media_reader_t<mpeg_ps_frame>
{
   public:
    void on_tracks(media_track_snapshot_ptr) override { consume(); }
    void on_end() override { ++ends; }

   protected:
    void on_read_ready(media_track_snapshot_ptr, bool) override { consume(); }

   private:
    void consume()
    {
        while (auto entry = read())
        {
            frames.push_back(std::move(entry->frame));
        }
    }

   public:
    std::vector<mpeg_ps_frame> frames;
    int ends{};
};

std::vector<media_track> tracks(codec_id video, codec_id audio)
{
    return {
        {.id = 1, .kind = media_kind::video, .codec = video, .clock_rate = 90'000, .codec_config = {}},
        {.id = 2,
         .kind = media_kind::audio,
         .codec = audio,
         .clock_rate = audio == codec_id::aac ? 44'100U : 8'000U,
         .channel_count = 1,
         .codec_config = audio == codec_id::aac ? std::vector<std::uint8_t>{0x12, 0x08} : std::vector<std::uint8_t>{}},
    };
}

int collect_legacy(void* param, int, const void* packet, int bytes, std::uint32_t, int)
{
    rtp_packet_t decoded{};
    require(rtp_packet_deserialize(&decoded, packet, bytes) == 0, "legacy RTP parse");
    require(decoded.rtp.m == 0, "PS marker follows existing byte-stream profile");
    auto& output = *static_cast<std::vector<std::uint8_t>*>(param);
    const auto* begin = static_cast<const std::uint8_t*>(decoded.payload);
    output.insert(output.end(), begin, begin + decoded.payloadlen);
    return 0;
}

void test_shared_bytes(codec_id video, codec_id audio)
{
    worker_context worker;
    auto source = std::make_shared<media_stream>("test/ps", worker);
    require(source->set_tracks(tracks(video, audio)), "source tracks");
    auto output = source->ps_output();
    require(output && source->ps_output() == output, "source owns exactly one PS preparation");
    auto first = std::make_shared<ps_reader>();
    auto second = std::make_shared<ps_reader>();
    output->stream()->add_reader(first, worker);
    output->stream()->add_reader(second, worker);
    drain(worker);

    std::vector<std::uint8_t> legacy_bytes;
    auto* legacy = rtsp_muxer_create(collect_legacy, &legacy_bytes);
    const auto payload = rtsp_muxer_add_payload(legacy, "RTP/AVP", 90'000, 96, "PS", 1, 1, 0, nullptr, 0);
    const auto v = rtsp_muxer_add_media(legacy, payload, video == codec_id::h264 ? RTP_PAYLOAD_H264 : RTP_PAYLOAD_H265, nullptr, 0);
    const auto a = rtsp_muxer_add_media(legacy,
                                        payload,
                                        audio == codec_id::aac     ? RTP_PAYLOAD_MP4A
                                        : audio == codec_id::g711a ? RTP_PAYLOAD_PCMA
                                                                   : RTP_PAYLOAD_PCMU,
                                        nullptr,
                                        0);
    require(payload >= 0 && v >= 0 && a >= 0, "legacy PS tracks");
    for (int i = 0; i < 66; ++i)
    {
        const bool is_video = i % 2 == 0;
        auto data = std::vector<std::uint8_t>(is_video ? 70'000U : 160U, 0x55);
        if (is_video)
        {
            const std::array<std::uint8_t, 6> prefix{0, 0, 0, 1, static_cast<std::uint8_t>(video == codec_id::h264 ? 0x65 : 0x26), 1};
            std::ranges::copy(prefix, data.begin());
        }
        else if (audio == codec_id::aac)
        {
            const std::array<std::uint8_t, 7> adts{0xff, 0xf1, 0x50, 0x40, 0x14, 0x1f, 0xfc};
            std::ranges::copy(adts, data.begin());
        }
        const auto dts = 80 + i * 20;
        const auto pts = dts + (is_video ? 40 : 0);
        const bool key = is_video && i % 30 == 0;
        legacy_bytes.clear();
        require(rtsp_muxer_input(legacy, is_video ? v : a, pts, dts, data.data(), static_cast<int>(data.size()), key ? 1 : 0) == 0,
                "legacy PS input");
        source->publish({.track = is_video ? 1U : 2U,
                         .dts_ns = static_cast<std::int64_t>(dts) * 1'000'000,
                         .pts_ns = static_cast<std::int64_t>(pts) * 1'000'000,
                         .key_frame = key,
                         .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(data))});
        drain(worker);
        require(first->frames.size() == static_cast<std::size_t>(i + 1) && second->frames.size() == first->frames.size(), "both readers progress");
        require(first->frames.back().payload == second->frames.back().payload, "readers share immutable PS bytes");
        require(*first->frames.back().payload == legacy_bytes, "PS/PES/PSM/SCR/PTS/DTS bytes match existing muxer");
    }
    rtsp_muxer_destroy(legacy);
    auto late = std::make_shared<ps_reader>();
    output->stream()->add_reader(late, worker);
    drain(worker);
    require(late->frames.size() == 6 && late->frames.front().key_frame, "late reader starts at latest GOP");
    require(late->frames.front().payload == first->frames[60].payload, "late reader reuses retained PS");

    auto config = source->tracks()[0];
    config.codec_config.push_back(0x01);
    require(source->update_track(config), "video config change");
    const auto count = first->frames.size();
    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{0, 0, 0, 1, 0x65, 1});
    source->publish({.track = 1, .dts_ns = 2'000'000'000, .pts_ns = 2'000'000'000, .key_frame = false, .payload = bytes});
    drain(worker);
    require(first->frames.size() == count, "new config waits for key frame");
    source->publish({.track = 1, .dts_ns = 2'040'000'000, .pts_ns = 2'080'000'000, .key_frame = true, .payload = bytes});
    drain(worker);
    require(first->frames.size() == count + 1, "new config resumes at key frame");

    source->end();
    source->end();
    drain(worker);
    require(first->ends == 1 && second->ends == 1 && late->ends == 1, "source end is idempotent");
    require(!source->ps_output(), "ended source cannot start output");
    auto replacement = std::make_shared<media_stream>(source->name(), worker);
    require(replacement->set_tracks(tracks(video, audio)), "replacement tracks");
    auto next = replacement->ps_output();
    require(next != output && next->stream() != output->stream(), "replacement owns new processor and history");
    auto next_reader = std::make_shared<ps_reader>();
    next->stream()->add_reader(next_reader, worker);
    drain(worker);
    require(next_reader->frames.empty(), "replacement does not replay previous generation");
    replacement->end();
    drain(worker);
    worker.request_stop();
    worker.run();
}

void test_independent_rtp()
{
    worker_context worker;
    auto source = std::make_shared<media_stream>("test/rtp", worker);
    require(source->set_tracks(tracks(codec_id::h264, codec_id::g711a)), "RTP source tracks");
    struct capture
    {
        std::vector<std::uint16_t> sequences;
        std::vector<std::uint32_t> timestamps;
        std::vector<std::uint8_t> ps;
    };
    std::array<capture, 2> received;
    std::array<std::shared_ptr<gb28181_rtp_sender>, 2> senders;
    for (std::size_t i = 0; i < senders.size(); ++i)
    {
        senders[i] = std::make_shared<gb28181_rtp_sender>(
            worker,
            source,
            static_cast<std::uint8_t>(96 + i),
            static_cast<std::uint32_t>(1234 + i),
            [&, i](std::vector<std::uint8_t> packet)
            {
                rtp_packet_t rtp{};
                require(rtp_packet_deserialize(&rtp, packet.data(), static_cast<int>(packet.size())) == 0, "RTP parse");
                require(rtp.rtp.ssrc == 1234 + i && rtp.rtp.pt == 96 + i && rtp.rtp.m == 0, "session SSRC/PT and PS marker");
                auto& out = received[i];
                if (!out.sequences.empty())
                {
                    require(static_cast<std::uint16_t>(out.sequences.back() + 1) == rtp.rtp.seq, "session sequence continuity");
                }
                out.sequences.push_back(static_cast<std::uint16_t>(rtp.rtp.seq));
                out.timestamps.push_back(rtp.rtp.timestamp);
                const auto* begin = static_cast<const std::uint8_t*>(rtp.payload);
                out.ps.insert(out.ps.end(), begin, begin + rtp.payloadlen);
            },
            []() {});
    }
    require(senders[0]->startup(), "first sender startup");
    drain(worker);
    auto payload = std::make_shared<std::vector<std::uint8_t>>(30'000, 0x55);
    (*payload)[0] = 0;
    (*payload)[1] = 0;
    (*payload)[2] = 0;
    (*payload)[3] = 1;
    (*payload)[4] = 0x65;
    source->publish({.track = 1, .dts_ns = 1'000'000'000, .pts_ns = 1'040'000'000, .key_frame = true, .payload = payload});
    drain(worker);
    require(senders[1]->startup(), "late sender startup");
    drain(worker);
    require(received[0].ps == received[1].ps && received[0].sequences.size() > 1, "fragmented PS payload shared semantically");
    const auto first_timestamp = received[1].timestamps.front();
    const auto count = received[0].sequences.size();
    senders[0]->shutdown();
    drain(worker);
    source->publish({.track = 1, .dts_ns = 1'040'000'000, .pts_ns = 1'080'000'000, .key_frame = false, .payload = payload});
    drain(worker);
    require(received[0].sequences.size() == count && received[1].sequences.size() == count * 2, "one sender shutdown does not stop another");
    require(received[1].timestamps.back() - first_timestamp == 3'600, "session media clock progresses independently");
    senders[1]->shutdown();
    source->end();
    drain(worker);
    worker.request_stop();
    worker.run();
}

void test_cancel_bootstrap()
{
    for (const bool prepared : {false, true})
    {
        worker_context source_worker;
        worker_context sender_worker;
        auto source = std::make_shared<media_stream>("test/cancel", source_worker);
        require(source->set_tracks(tracks(codec_id::h264, codec_id::aac)), "cancel source tracks");
        int callbacks = 0;
        auto sender = std::make_shared<gb28181_rtp_sender>(sender_worker,
                                                        source,
                                                        96,
                                                        42,
                                                        [&](std::vector<std::uint8_t>) { ++callbacks; },
                                                        [&]() { ++callbacks; },
                                                        [&]() { ++callbacks; });
        require(sender->startup(), "cancel sender startup");
        if (prepared)
        {
            drain(source_worker);
        }
        sender->shutdown();
        sender->shutdown();
        std::weak_ptr<gb28181_rtp_sender> released = sender;
        sender.reset();
        source->end();
        drain(source_worker);
        drain(sender_worker);
        drain(source_worker);
        require(callbacks == 0 && released.expired(), "cancelled cross-worker bootstrap releases without callbacks");
        source_worker.request_stop();
        sender_worker.request_stop();
        source_worker.run();
        sender_worker.run();
    }
}

void test_reader_owned_batch_progression()
{
    worker_context worker;
    auto source = std::make_shared<media_stream>("test/ps-batch", worker);
    require(source->set_tracks(tracks(codec_id::h264, codec_id::g711a)), "batch source tracks");
    auto output = source->ps_output();
    require(output != nullptr, "batch PS output");

    const auto payload = std::make_shared<const std::vector<std::uint8_t>>(
        std::initializer_list<std::uint8_t>{0, 0, 0, 1, 0x65, 1});
    for (int index = 0; index < 150; ++index)
    {
        source->publish({.track = 1,
                         .dts_ns = static_cast<std::int64_t>(index) * 40'000'000,
                         .pts_ns = static_cast<std::int64_t>(index) * 40'000'000,
                         .key_frame = index == 0,
                         .payload = payload});
    }

    auto reader = std::make_shared<ps_reader>();
    output->stream()->add_reader(reader, worker);
    drain(worker);
    require(reader->frames.size() == 150, "reader advances across the 128-entry history batch boundary");
    source->end();
    drain(worker);
    require(reader->ends == 1, "batch reader receives source end");
    worker.request_stop();
    worker.run();
}
}    // namespace

int main()
{
    try
    {
        for (const auto video : {codec_id::h264, codec_id::h265})
        {
            for (const auto audio : {codec_id::aac, codec_id::g711a, codec_id::g711u})
            {
                test_shared_bytes(video, audio);
            }
        }
        test_independent_rtp();
        test_cancel_bootstrap();
        test_reader_owned_batch_progression();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
