#ifndef MEDIA_CODEC_AAC_OPUS_TRANSCODER_H
#define MEDIA_CODEC_AAC_OPUS_TRANSCODER_H

#include <cstdint>
#include <span>
#include <vector>

extern "C"
{
struct AVAudioFifo;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
}

namespace media_server
{

struct opus_audio_packet
{
    std::vector<std::uint8_t> payload;
    std::uint32_t sample_count{};
};

class aac_opus_transcoder final
{
public:
    aac_opus_transcoder() = default;
    ~aac_opus_transcoder();

    bool start(std::span<const std::uint8_t> audio_specific_config, int output_channel_count);
    bool transcode(
        std::span<const std::uint8_t> adts_frame,
        std::vector<opus_audio_packet>& packets);

private:
    void cleanup();
    void drain_encoder();
    bool configure_resampler(const AVFrame& frame);
    bool resample(const AVFrame& frame);
    bool encode_available(std::vector<opus_audio_packet>& packets);
    bool receive_encoded(std::vector<opus_audio_packet>& packets);

    AVCodecContext* decoder_{};
    AVCodecContext* encoder_{};
    SwrContext* resampler_{};
    AVAudioFifo* fifo_{};
    AVFrame* decoded_frame_{};
    AVFrame* encoded_frame_{};
    AVPacket* input_packet_{};
    AVPacket* output_packet_{};
    std::int64_t next_pts_{};
    int encoder_frame_samples_{};
};

}    // namespace media_server

#endif
