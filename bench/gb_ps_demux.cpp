#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>

extern "C"
{
#include "mpeg-ps.h"
#include "mpeg-types.h"
#include "mpeg-proto.h"
#include "rtp-packet.h"
}

namespace
{

constexpr std::int64_t mpeg_timestamp_period = std::int64_t{1} << 33;
constexpr std::size_t max_reported_errors = 20;
constexpr std::size_t record_header_bytes = 2;

struct track_state
{
    std::string codec;
    std::ofstream es;
    std::uint64_t frames{};
    std::uint64_t bytes{};
    std::optional<std::int64_t> last_raw_dts;
    std::int64_t wrap_offset{};
    std::optional<std::int64_t> last_dts;
};

struct demux_state
{
    std::string prefix;
    std::ofstream index;
    std::map<int, int> stream_types;
    std::uint64_t stream_maps{};
    track_state video;
    track_state audio;
    std::uint64_t key_frames{};
    std::vector<std::string> errors;
    std::uint64_t error_count{};

    void fail(std::string message)
    {
        if (errors.size() < max_reported_errors)
        {
            errors.push_back(std::move(message));
        }
        ++error_count;
    }
};

std::string_view codec_name(int stream_type)
{
    switch (stream_type)
    {
        case PSI_STREAM_H264:
            return "h264";
        case PSI_STREAM_H265:
            return "hevc";
        case PSI_STREAM_AAC:
            return "aac";
        case PSI_STREAM_AUDIO_G711A:
            return "alaw";
        case PSI_STREAM_AUDIO_G711U:
            return "mulaw";
        default:
            return {};
    }
}

bool is_video(std::string_view codec) { return codec == "h264" || codec == "hevc"; }

std::int64_t unwrap(track_state& track, std::int64_t raw)
{
    if (track.last_raw_dts && raw < *track.last_raw_dts && *track.last_raw_dts - raw > mpeg_timestamp_period / 2)
    {
        track.wrap_offset += mpeg_timestamp_period;
    }
    track.last_raw_dts = raw;
    return raw + track.wrap_offset;
}

bool annex_b(const std::uint8_t* data, std::size_t bytes)
{
    return (bytes >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
           (bytes >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

std::optional<std::uint64_t> adts_frames(const std::uint8_t* data, std::size_t bytes)
{
    std::uint64_t frames{};
    std::size_t offset{};
    while (offset < bytes)
    {
        if (bytes - offset < 7 || data[offset] != 0xFF || (data[offset + 1] & 0xF6U) != 0xF0U)
        {
            return std::nullopt;
        }
        const auto length = (static_cast<std::size_t>(data[offset + 3] & 3U) << 11U) | (static_cast<std::size_t>(data[offset + 4]) << 3U) |
                            (static_cast<std::size_t>(data[offset + 5]) >> 5U);
        if (length < 7 || length > bytes - offset)
        {
            return std::nullopt;
        }
        frames += 1U + (data[offset + 6] & 3U);
        offset += length;
    }
    return frames;
}

void on_stream(void* param, int stream, int codecid, const void*, int, int)
{
    auto& state = *static_cast<demux_state*>(param);
    ++state.stream_maps;
    if (codec_name(codecid).empty())
    {
        state.fail("unsupported stream type " + std::to_string(codecid) + " for stream " + std::to_string(stream));
        return;
    }
    const auto [iterator, inserted] = state.stream_types.try_emplace(stream, codecid);
    if (!inserted && iterator->second != codecid)
    {
        state.fail("stream type change for stream " + std::to_string(stream));
    }
}

int on_packet(void* param, int stream, int codecid, int flags, std::int64_t pts, std::int64_t dts, const void* data, std::size_t bytes)
{
    auto& state = *static_cast<demux_state*>(param);
    const auto mapped = state.stream_types.find(stream);
    if (mapped == state.stream_types.end())
    {
        state.fail("packet for stream " + std::to_string(stream) + " without a stream map entry");
        return 0;
    }
    if (mapped->second != codecid)
    {
        state.fail("packet codec " + std::to_string(codecid) + " differs from stream map type " + std::to_string(mapped->second));
        return 0;
    }
    const std::string codec{codec_name(codecid)};
    auto& track = is_video(codec) ? state.video : state.audio;
    if (track.codec.empty())
    {
        track.codec = codec;
        track.es.open(state.prefix + "." + codec, std::ios::binary);
        if (!track.es)
        {
            state.fail("cannot open " + state.prefix + "." + codec);
            return -1;
        }
    }
    else if (track.codec != codec)
    {
        state.fail("second " + std::string{is_video(codec) ? "video" : "audio"} + " codec " + codec);
        return 0;
    }
    const auto* bytes_data = static_cast<const std::uint8_t*>(data);
    std::uint64_t units = 1;
    if (is_video(codec) && !annex_b(bytes_data, bytes))
    {
        state.fail("video frame " + std::to_string(track.frames) + " is not Annex-B");
    }
    if (codec == "aac")
    {
        const auto frames = adts_frames(bytes_data, bytes);
        if (!frames)
        {
            state.fail("audio frame " + std::to_string(track.frames) + " is not a whole number of ADTS frames");
        }
        units = frames.value_or(0);
    }
    if ((codec == "alaw" || codec == "mulaw"))
    {
        units = bytes;
    }
    if (bytes == 0)
    {
        state.fail("empty frame");
    }
    if (pts == PTS_NO_VALUE || dts == PTS_NO_VALUE)
    {
        state.fail("frame " + std::to_string(track.frames) + " without timestamps");
        return 0;
    }
    auto offset = (pts - dts) % mpeg_timestamp_period;
    offset += offset < -mpeg_timestamp_period / 2 ? mpeg_timestamp_period : 0;
    const auto unwrapped_dts = unwrap(track, dts);
    const auto unwrapped_pts = unwrapped_dts + offset;
    if (track.last_dts && unwrapped_dts <= *track.last_dts)
    {
        state.fail(std::string{is_video(codec) ? "video" : "audio"} + " dts not increasing at frame " + std::to_string(track.frames));
    }
    if (offset < 0)
    {
        state.fail("pts before dts at frame " + std::to_string(track.frames));
    }
    track.last_dts = unwrapped_dts;
    const bool key = (flags & MPEG_FLAG_IDR_FRAME) != 0;
    state.key_frames += key && is_video(codec) ? 1U : 0U;
    state.index << (is_video(codec) ? "video," : "audio,") << unwrapped_pts << ',' << unwrapped_dts << ',' << (key ? 1 : 0) << ',' << track.bytes
                << ',' << bytes << ',' << units << '\n';
    track.es.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!track.es || !state.index)
    {
        state.fail("write failed");
        return -1;
    }
    ++track.frames;
    track.bytes += bytes;
    return 0;
}

std::uint16_t read_u16(const std::uint8_t* data) { return static_cast<std::uint16_t>((data[0] << 8U) | data[1]); }

}    // namespace

int main(int argc, char** argv)
{
    std::string input;
    std::string prefix;
    int payload_type = 96;
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string_view argument = argv[index];
        if (argument == "--input")
        {
            input = argv[index + 1];
        }
        else if (argument == "--output-prefix")
        {
            prefix = argv[index + 1];
        }
        else if (argument == "--payload-type")
        {
            payload_type = std::stoi(argv[index + 1]);
        }
    }
    if (input.empty() || prefix.empty() || argc % 2 == 0)
    {
        std::cerr
            << "usage: gb_ps_demux --input DUMP --output-prefix PREFIX [--payload-type 96]\n"
               "DUMP holds RTP packets each preceded by a 16-bit big-endian length (RFC 4571 framing). Writes PREFIX.<codec> elementary "
               "streams and PREFIX.csv (kind,pts,dts,key,offset,bytes,units; 90 kHz timestamps unwrapped past 33 bits; units are ADTS frames or "
               "G.711 samples) and prints a JSON summary. Codecs come only from the PS stream map.\n";
        return 2;
    }
    std::ifstream file(input, std::ios::binary);
    const std::vector<std::uint8_t> dump{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    demux_state state;
    state.prefix = prefix;
    state.index.open(prefix + ".csv");
    if (!file.is_open() || !state.index)
    {
        std::cerr << "cannot read " << input << " or write " << prefix << ".csv\n";
        return 2;
    }
    auto* demuxer = ps_demuxer_create(&on_packet, &state);
    ps_demuxer_notify_t notify{&on_stream};
    ps_demuxer_set_notify(demuxer, &notify, &state);

    std::vector<std::uint8_t> pending;
    std::uint64_t packets{};
    std::uint64_t sequence_gaps{};
    std::optional<std::uint16_t> sequence;
    std::optional<std::uint32_t> ssrc;
    std::size_t offset{};
    while (offset < dump.size())
    {
        if (dump.size() - offset < record_header_bytes || dump.size() - offset - record_header_bytes < read_u16(dump.data() + offset))
        {
            state.fail("truncated RTP record at byte " + std::to_string(offset));
            break;
        }
        const auto length = read_u16(dump.data() + offset);
        const auto* data = dump.data() + offset + record_header_bytes;
        offset += record_header_bytes + length;
        rtp_packet_t packet{};
        if (rtp_packet_deserialize(&packet, data, length) != 0 || packet.rtp.v != 2 || packet.payloadlen <= 0)
        {
            state.fail("invalid RTP packet " + std::to_string(packets));
            ++packets;
            continue;
        }
        if (static_cast<int>(packet.rtp.pt) != payload_type || (ssrc && *ssrc != packet.rtp.ssrc))
        {
            state.fail("unexpected payload type or SSRC in RTP packet " + std::to_string(packets));
        }
        if (sequence && static_cast<std::uint16_t>(packet.rtp.seq - *sequence) != 1U)
        {
            ++sequence_gaps;
            state.fail("RTP sequence " + std::to_string(packet.rtp.seq) + " after " + std::to_string(*sequence));
        }
        sequence = static_cast<std::uint16_t>(packet.rtp.seq);
        ssrc = packet.rtp.ssrc;
        ++packets;
        const auto* payload = static_cast<const std::uint8_t*>(packet.payload);
        pending.insert(pending.end(), payload, payload + packet.payloadlen);
        const auto consumed = ps_demuxer_input(demuxer, pending.data(), pending.size());
        if (consumed < 0)
        {
            state.fail("PS demuxer error " + std::to_string(consumed) + " at RTP packet " + std::to_string(packets - 1));
            pending.clear();
            continue;
        }
        pending.erase(pending.begin(), pending.begin() + consumed);
    }
    ps_demuxer_destroy(demuxer);
    for (auto* output : {&state.video.es, &state.audio.es, &state.index})
    {
        if (output->is_open())
        {
            output->close();
            if (output->fail())
            {
                state.fail("closing outputs failed");
            }
        }
    }

    std::cout << "{\"rtp_packets\": " << packets << ", \"rtp_sequence_gaps\": " << sequence_gaps << ", \"stream_map_entries\": " << state.stream_maps
              << ", \"video_codec\": \"" << state.video.codec << "\", \"audio_codec\": \"" << state.audio.codec
              << "\", \"video_frames\": " << state.video.frames << ", \"key_frames\": " << state.key_frames
              << ", \"audio_frames\": " << state.audio.frames << ", \"audio_bytes\": " << state.audio.bytes
              << ", \"trailing_bytes\": " << pending.size() << ", \"error_count\": " << state.error_count << ", \"errors\": [";
    for (std::size_t index = 0; index < state.errors.size(); ++index)
    {
        std::cout << (index == 0 ? "\"" : ", \"") << state.errors[index] << '"';
    }
    std::cout << "]}\n";
    return state.error_count == 0 ? 0 : 1;
}
