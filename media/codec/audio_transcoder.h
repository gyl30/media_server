#ifndef MEDIA_CODEC_AUDIO_TRANSCODER_H
#define MEDIA_CODEC_AUDIO_TRANSCODER_H

#include <memory>
#include <vector>
#include <cstdint>

#include "media/core/media_types.h"

namespace media_server
{

struct audio_transcoder_format
{
    codec_id codec{};
    std::uint32_t sample_rate{};
    std::uint16_t channel_count{};
};

struct audio_transcoder_config
{
    audio_transcoder_format input;
    audio_transcoder_format output;
    std::vector<std::uint8_t> input_codec_config;
    int output_bit_rate{};
    int output_cutoff{};
};

class audio_transcoder final
{
   public:
    audio_transcoder();
    ~audio_transcoder();

    bool startup(const audio_transcoder_config& config);
    void shutdown();
    bool transcode(const media_frame& input, std::vector<media_frame>& output);
    bool flush(std::vector<media_frame>& output);

   private:
    struct state;

    bool receive_decoded(std::vector<media_frame>& output, bool draining);
    bool configure_resampler();
    bool resample_decoded();
    int convert_samples(const std::uint8_t* const* input, int input_samples, int output_capacity);
    bool encode_available(std::vector<media_frame>& output);
    bool encode_fifo_frame(int sample_count, std::vector<media_frame>& output);
    bool prepare_encoded_frame(int sample_count);
    bool receive_encoded(std::vector<media_frame>& output, bool draining);

    std::unique_ptr<state> state_;
};

}    // namespace media_server

#endif
