#include <span>
#include <cerrno>
#include <limits>
#include <string>
#include <cstring>
#include <optional>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/codec/audio_transcoder.h"

extern "C"
{
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace media_server
{
namespace
{

constexpr AVRational nanoseconds_time_base{1, 1'000'000'000};
constexpr double timestamp_compensation_threshold_seconds = 0.001;

std::string ffmpeg_error(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, buffer, sizeof(buffer)) != 0)
    {
        return std::to_string(error);
    }
    return buffer;
}

std::optional<std::span<const std::uint8_t>> adts_payload(std::span<const std::uint8_t> frame)
{
    constexpr std::size_t adts_header_size = 7;
    constexpr std::size_t adts_crc_header_size = 9;

    if (frame.size() < adts_header_size || frame[0] != 0xffU || (frame[1] & 0xf6U) != 0xf0U)
    {
        return std::nullopt;
    }

    const std::size_t frame_size =
        (static_cast<std::size_t>(frame[3] & 0x03U) << 11U) | (static_cast<std::size_t>(frame[4]) << 3U) | (static_cast<std::size_t>(frame[5]) >> 5U);
    if (frame_size != frame.size() || (frame[6] & 0x03U) != 0U)
    {
        return std::nullopt;
    }

    const std::size_t header_size = (frame[1] & 0x01U) != 0U ? adts_header_size : adts_crc_header_size;
    if (frame.size() <= header_size)
    {
        return std::nullopt;
    }
    return frame.subspan(header_size);
}

}    // namespace

struct audio_transcoder::state
{
    AVCodecContext* decoder{};
    AVCodecContext* encoder{};
    SwrContext* resampler{};
    AVAudioFifo* fifo{};
    AVFrame* decoded_frame{};
    AVFrame* encoded_frame{};
    AVPacket* input_packet{};
    AVPacket* output_packet{};
    std::int64_t next_encoder_pts{};
    std::int64_t next_output_pts_ns{};
    track_id output_track{};
    int encoder_frame_samples{};
    int encoded_frame_capacity{};
    bool timeline_started{};
    bool input_ended{};
    bool flushed{};
};

audio_transcoder::audio_transcoder() = default;

audio_transcoder::~audio_transcoder() { shutdown(); }

bool audio_transcoder::initialize_decoder(const audio_transcoder_config& config)
{
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (decoder == nullptr)
    {
        spdlog::error("audio transcoder aac decoder not found");
        return false;
    }

    state_->decoder = avcodec_alloc_context3(decoder);
    if (state_->decoder == nullptr)
    {
        spdlog::error("audio transcoder decoder context allocate failed");
        return false;
    }

    state_->decoder->sample_rate = static_cast<int>(config.input.sample_rate);
    state_->decoder->pkt_timebase = AVRational{1, state_->decoder->sample_rate};
    av_channel_layout_default(&state_->decoder->ch_layout, config.input.channel_count);
    state_->decoder->extradata = static_cast<std::uint8_t*>(av_mallocz(config.input_codec_config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (state_->decoder->extradata == nullptr)
    {
        spdlog::error("audio transcoder decoder extradata allocate failed");
        return false;
    }
    state_->decoder->extradata_size = static_cast<int>(config.input_codec_config.size());
    std::memcpy(state_->decoder->extradata, config.input_codec_config.data(), config.input_codec_config.size());

    int result = avcodec_open2(state_->decoder, decoder, nullptr);
    if (result < 0)
    {
        spdlog::error("audio transcoder decoder open failed {}", ffmpeg_error(result));
        return false;
    }
    return true;
}

bool audio_transcoder::initialize_encoder(const audio_transcoder_config& config)
{
    const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
    if (encoder == nullptr)
    {
        encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (encoder == nullptr)
    {
        spdlog::error("audio transcoder opus encoder not found");
        return false;
    }

    const void* sample_formats{};
    int result = avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &sample_formats, nullptr);
    if (result < 0 || sample_formats == nullptr)
    {
        spdlog::error("audio transcoder encoder sample formats query failed {}", ffmpeg_error(result));
        return false;
    }

    state_->encoder = avcodec_alloc_context3(encoder);
    if (state_->encoder == nullptr)
    {
        spdlog::error("audio transcoder encoder context allocate failed");
        return false;
    }

    av_channel_layout_default(&state_->encoder->ch_layout, config.output.channel_count);
    state_->encoder->sample_rate = static_cast<int>(config.output.sample_rate);
    state_->encoder->sample_fmt = static_cast<const AVSampleFormat*>(sample_formats)[0];
    state_->encoder->bit_rate = config.output_bit_rate;
    state_->encoder->cutoff = config.output_cutoff;
    state_->encoder->time_base = AVRational{1, state_->encoder->sample_rate};
    state_->encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    result = avcodec_open2(state_->encoder, encoder, nullptr);
    if (result < 0)
    {
        spdlog::error("audio transcoder encoder open failed {}", ffmpeg_error(result));
        return false;
    }

    state_->encoder_frame_samples = state_->encoder->frame_size;
    if (state_->encoder_frame_samples <= 0 && (encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) == 0)
    {
        spdlog::error("audio transcoder encoder invalid frame size {}", state_->encoder_frame_samples);
        return false;
    }
    return true;
}

bool audio_transcoder::allocate_buffers()
{
    state_->fifo =
        av_audio_fifo_alloc(state_->encoder->sample_fmt, state_->encoder->ch_layout.nb_channels, std::max(state_->encoder_frame_samples, 1));
    state_->decoded_frame = av_frame_alloc();
    state_->encoded_frame = av_frame_alloc();
    state_->input_packet = av_packet_alloc();
    state_->output_packet = av_packet_alloc();
    if (state_->fifo == nullptr || state_->decoded_frame == nullptr || state_->encoded_frame == nullptr || state_->input_packet == nullptr ||
        state_->output_packet == nullptr)
    {
        spdlog::error("audio transcoder buffer allocate failed");
        return false;
    }
    return true;
}

bool audio_transcoder::startup(const audio_transcoder_config& config)
{
    shutdown();

    if (config.input.codec != codec_id::aac || config.output.codec != codec_id::opus || config.input.sample_rate == 0 ||
        config.input.sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || config.input.channel_count == 0 ||
        config.output.sample_rate == 0 || config.output.sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        (config.output.channel_count != 1 && config.output.channel_count != 2) || config.input_codec_config.empty() ||
        config.input_codec_config.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || config.output_bit_rate < 6'000 ||
        config.output_bit_rate > 510'000 || config.output_cutoff < 4'000 || config.output_cutoff > 20'000)
    {
        spdlog::error("audio transcoder invalid config input {} {}hz {}ch output {} {}hz {}ch bitrate {} cutoff {}",
                      to_string(config.input.codec),
                      config.input.sample_rate,
                      config.input.channel_count,
                      to_string(config.output.codec),
                      config.output.sample_rate,
                      config.output.channel_count,
                      config.output_bit_rate,
                      config.output_cutoff);
        return false;
    }

    state_ = std::make_unique<state>();
    if (!initialize_decoder(config) || !initialize_encoder(config) || !allocate_buffers())
    {
        shutdown();
        return false;
    }

    spdlog::debug("audio transcoder started input {} {}hz {}ch output {} {}hz {}ch encoder {} frame_samples {} bitrate {} cutoff {}",
                  to_string(config.input.codec),
                  config.input.sample_rate,
                  config.input.channel_count,
                  to_string(config.output.codec),
                  config.output.sample_rate,
                  config.output.channel_count,
                  state_->encoder->codec->name,
                  state_->encoder_frame_samples,
                  state_->encoder->bit_rate,
                  state_->encoder->cutoff);
    return true;
}

void audio_transcoder::shutdown()
{
    if (!state_)
    {
        return;
    }
    if (state_->input_packet != nullptr)
    {
        av_packet_free(&state_->input_packet);
    }
    if (state_->output_packet != nullptr)
    {
        av_packet_free(&state_->output_packet);
    }
    if (state_->decoded_frame != nullptr)
    {
        av_frame_free(&state_->decoded_frame);
    }
    if (state_->encoded_frame != nullptr)
    {
        av_frame_free(&state_->encoded_frame);
    }
    if (state_->fifo != nullptr)
    {
        av_audio_fifo_free(state_->fifo);
        state_->fifo = nullptr;
    }
    if (state_->resampler != nullptr)
    {
        swr_free(&state_->resampler);
    }
    if (state_->decoder != nullptr)
    {
        avcodec_free_context(&state_->decoder);
    }
    if (state_->encoder != nullptr)
    {
        avcodec_free_context(&state_->encoder);
    }
    state_.reset();
}

bool audio_transcoder::transcode(const media_frame& input, std::vector<media_frame>& output)
{
    if (!state_ || state_->input_ended || !input.payload || input.payload->empty() ||
        input.payload->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    const auto payload = adts_payload(*input.payload);
    if (!payload || payload->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        spdlog::debug("audio transcoder invalid aac adts frame size {}", input.payload->size());
        return false;
    }

    av_packet_unref(state_->input_packet);
    int result = av_new_packet(state_->input_packet, static_cast<int>(payload->size()));
    if (result < 0)
    {
        spdlog::error("audio transcoder input packet allocate failed {}", ffmpeg_error(result));
        return false;
    }
    std::memcpy(state_->input_packet->data, payload->data(), payload->size());
    state_->input_packet->pts = av_rescale_q(input.pts_ns, nanoseconds_time_base, state_->decoder->pkt_timebase);
    state_->input_packet->dts = av_rescale_q(input.dts_ns, nanoseconds_time_base, state_->decoder->pkt_timebase);

    result = avcodec_send_packet(state_->decoder, state_->input_packet);
    if (result == AVERROR(EAGAIN))
    {
        if (!receive_decoded(output, false))
        {
            return false;
        }
        result = avcodec_send_packet(state_->decoder, state_->input_packet);
    }
    if (result < 0)
    {
        spdlog::debug("audio transcoder decode send failed {}", ffmpeg_error(result));
        return false;
    }
    if (!state_->timeline_started)
    {
        state_->next_output_pts_ns = input.pts_ns;
        state_->output_track = input.track;
        state_->timeline_started = true;
    }
    return receive_decoded(output, false);
}

bool audio_transcoder::flush(std::vector<media_frame>& output)
{
    if (!state_)
    {
        return false;
    }
    if (state_->flushed)
    {
        return true;
    }
    state_->input_ended = true;

    int result = avcodec_send_packet(state_->decoder, nullptr);
    if (result == AVERROR(EAGAIN))
    {
        if (!receive_decoded(output, false))
        {
            return false;
        }
        result = avcodec_send_packet(state_->decoder, nullptr);
    }
    if (result < 0 && result != AVERROR_EOF)
    {
        spdlog::error("audio transcoder decoder flush send failed {}", ffmpeg_error(result));
        return false;
    }
    if (result >= 0 && !receive_decoded(output, true))
    {
        return false;
    }

    if (state_->resampler != nullptr)
    {
        while (true)
        {
            const int capacity = swr_get_out_samples(state_->resampler, 0);
            if (capacity < 0)
            {
                spdlog::error("audio transcoder resampler flush capacity failed {}", ffmpeg_error(capacity));
                return false;
            }
            if (capacity == 0)
            {
                break;
            }

            const int converted = convert_samples(nullptr, 0, capacity);
            if (converted < 0)
            {
                return false;
            }
            if (converted == 0)
            {
                break;
            }
        }
    }

    if (!encode_available(output))
    {
        return false;
    }

    const int remaining = av_audio_fifo_size(state_->fifo);
    if (remaining > 0)
    {
        if (state_->encoder_frame_samples > 0 && (state_->encoder->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) == 0)
        {
            spdlog::error("audio transcoder encoder rejects small last frame samples {}", remaining);
            return false;
        }
        if (!encode_fifo_frame(remaining, output))
        {
            return false;
        }
    }

    result = avcodec_send_frame(state_->encoder, nullptr);
    if (result == AVERROR(EAGAIN))
    {
        if (!receive_encoded(output, false))
        {
            return false;
        }
        result = avcodec_send_frame(state_->encoder, nullptr);
    }
    if (result < 0 && result != AVERROR_EOF)
    {
        spdlog::error("audio transcoder encoder flush send failed {}", ffmpeg_error(result));
        return false;
    }
    if (result >= 0 && !receive_encoded(output, true))
    {
        return false;
    }

    state_->flushed = true;
    return true;
}

bool audio_transcoder::receive_decoded(std::vector<media_frame>& output, bool draining)
{
    while (true)
    {
        av_frame_unref(state_->decoded_frame);
        const int result = avcodec_receive_frame(state_->decoder, state_->decoded_frame);
        if (result == AVERROR(EAGAIN))
        {
            return !draining;
        }
        if (result == AVERROR_EOF)
        {
            return true;
        }
        if (result < 0)
        {
            spdlog::debug("audio transcoder decode receive failed {}", ffmpeg_error(result));
            return false;
        }

        if (state_->resampler == nullptr && !configure_resampler())
        {
            return false;
        }
        if (!resample_decoded() || !encode_available(output))
        {
            return false;
        }
    }
}

bool audio_transcoder::configure_resampler()
{
    const auto& frame = *state_->decoded_frame;
    if (frame.sample_rate <= 0 || frame.ch_layout.nb_channels <= 0)
    {
        return false;
    }

    const int result = swr_alloc_set_opts2(&state_->resampler,
                                           &state_->encoder->ch_layout,
                                           state_->encoder->sample_fmt,
                                           state_->encoder->sample_rate,
                                           &frame.ch_layout,
                                           static_cast<AVSampleFormat>(frame.format),
                                           frame.sample_rate,
                                           0,
                                           nullptr);
    if (result < 0 || state_->resampler == nullptr)
    {
        spdlog::error("audio transcoder resampler allocate failed {}", ffmpeg_error(result));
        return false;
    }

    const int min_comp_result = av_opt_set_double(state_->resampler, "min_comp", timestamp_compensation_threshold_seconds, 0);
    const int min_hard_comp_result = av_opt_set_double(state_->resampler, "min_hard_comp", timestamp_compensation_threshold_seconds, 0);
    if (min_comp_result < 0 || min_hard_comp_result < 0)
    {
        spdlog::error("audio transcoder resampler timestamp compensation configure failed");
        swr_free(&state_->resampler);
        return false;
    }

    const int init_result = swr_init(state_->resampler);
    if (init_result < 0)
    {
        spdlog::error("audio transcoder resampler init failed {}", ffmpeg_error(init_result));
        swr_free(&state_->resampler);
        return false;
    }

    spdlog::debug("audio transcoder resampler started input {}hz {}ch output {}hz {}ch",
                  frame.sample_rate,
                  frame.ch_layout.nb_channels,
                  state_->encoder->sample_rate,
                  state_->encoder->ch_layout.nb_channels);
    return true;
}

bool audio_transcoder::resample_decoded()
{
    const auto& frame = *state_->decoded_frame;
    if (frame.sample_rate <= 0 || frame.nb_samples <= 0)
    {
        return false;
    }

    if (frame.pts == AV_NOPTS_VALUE)
    {
        return false;
    }

    const auto frame_sample_pts = av_rescale_q(frame.pts, state_->decoder->pkt_timebase, AVRational{1, frame.sample_rate});
    const auto resampler_pts = av_rescale(frame_sample_pts, state_->encoder->sample_rate, 1);
    swr_next_pts(state_->resampler, resampler_pts);

    const auto delayed_samples = swr_get_delay(state_->resampler, frame.sample_rate);
    const auto capacity = av_rescale_rnd(delayed_samples + frame.nb_samples, state_->encoder->sample_rate, frame.sample_rate, AV_ROUND_UP);
    if (capacity <= 0 || capacity > std::numeric_limits<int>::max())
    {
        return false;
    }

    const auto input_format = static_cast<AVSampleFormat>(frame.format);
    const auto plane_count = av_sample_fmt_is_planar(input_format) != 0 ? static_cast<std::size_t>(frame.ch_layout.nb_channels) : 1U;
    std::vector<const std::uint8_t*> input_data(plane_count);
    for (std::size_t index = 0; index < input_data.size(); ++index)
    {
        input_data[index] = frame.extended_data[index];
    }
    return convert_samples(input_data.data(), frame.nb_samples, static_cast<int>(capacity)) >= 0;
}

int audio_transcoder::convert_samples(const std::uint8_t* const* input, int input_samples, int output_capacity)
{
    AVFrame* converted = av_frame_alloc();
    if (converted == nullptr)
    {
        return AVERROR(ENOMEM);
    }
    converted->format = state_->encoder->sample_fmt;
    converted->sample_rate = state_->encoder->sample_rate;
    converted->nb_samples = output_capacity;
    const int layout_result = av_channel_layout_copy(&converted->ch_layout, &state_->encoder->ch_layout);
    if (layout_result < 0)
    {
        av_frame_free(&converted);
        return layout_result;
    }

    int result = av_frame_get_buffer(converted, 0);
    if (result < 0)
    {
        spdlog::error("audio transcoder resample frame allocate failed {}", ffmpeg_error(result));
        av_frame_free(&converted);
        return result;
    }

    result = swr_convert(state_->resampler, converted->extended_data, converted->nb_samples, input, input_samples);
    if (result < 0)
    {
        spdlog::debug("audio transcoder resample failed {}", ffmpeg_error(result));
        av_frame_free(&converted);
        return result;
    }
    if (result > 0)
    {
        const int written = av_audio_fifo_write(state_->fifo, reinterpret_cast<void**>(converted->extended_data), result);
        if (written != result)
        {
            spdlog::error("audio transcoder fifo write failed expected {} actual {}", result, written);
            av_frame_free(&converted);
            return AVERROR_UNKNOWN;
        }
    }

    av_frame_free(&converted);
    return result;
}

bool audio_transcoder::encode_available(std::vector<media_frame>& output)
{
    while (true)
    {
        const int available = av_audio_fifo_size(state_->fifo);
        const int sample_count = state_->encoder_frame_samples > 0 ? state_->encoder_frame_samples : available;
        if (available < sample_count || sample_count <= 0)
        {
            return true;
        }
        if (!encode_fifo_frame(sample_count, output))
        {
            return false;
        }
    }
}

bool audio_transcoder::encode_fifo_frame(int sample_count, std::vector<media_frame>& output)
{
    if (!prepare_encoded_frame(sample_count))
    {
        return false;
    }

    const int read = av_audio_fifo_read(state_->fifo, reinterpret_cast<void**>(state_->encoded_frame->extended_data), sample_count);
    if (read != sample_count)
    {
        spdlog::error("audio transcoder fifo read failed expected {} actual {}", sample_count, read);
        return false;
    }

    state_->encoded_frame->nb_samples = sample_count;
    state_->encoded_frame->pts = state_->next_encoder_pts;
    state_->next_encoder_pts += sample_count;

    int result = avcodec_send_frame(state_->encoder, state_->encoded_frame);
    if (result == AVERROR(EAGAIN))
    {
        if (!receive_encoded(output, false))
        {
            return false;
        }
        result = avcodec_send_frame(state_->encoder, state_->encoded_frame);
    }
    if (result < 0)
    {
        spdlog::debug("audio transcoder encode send failed {}", ffmpeg_error(result));
        return false;
    }
    return receive_encoded(output, false);
}

bool audio_transcoder::prepare_encoded_frame(int sample_count)
{
    if (state_->encoded_frame_capacity < sample_count)
    {
        av_frame_unref(state_->encoded_frame);
        state_->encoded_frame->format = state_->encoder->sample_fmt;
        state_->encoded_frame->sample_rate = state_->encoder->sample_rate;
        state_->encoded_frame->nb_samples = sample_count;
        if (av_channel_layout_copy(&state_->encoded_frame->ch_layout, &state_->encoder->ch_layout) < 0)
        {
            return false;
        }
        const int result = av_frame_get_buffer(state_->encoded_frame, 0);
        if (result < 0)
        {
            spdlog::error("audio transcoder encoder frame allocate failed {}", ffmpeg_error(result));
            return false;
        }
        state_->encoded_frame_capacity = sample_count;
    }

    const int result = av_frame_make_writable(state_->encoded_frame);
    if (result < 0)
    {
        spdlog::error("audio transcoder encoder frame writable failed {}", ffmpeg_error(result));
        return false;
    }
    return true;
}

bool audio_transcoder::receive_encoded(std::vector<media_frame>& output, bool draining)
{
    while (true)
    {
        av_packet_unref(state_->output_packet);
        const int result = avcodec_receive_packet(state_->encoder, state_->output_packet);
        if (result == AVERROR(EAGAIN))
        {
            return !draining;
        }
        if (result == AVERROR_EOF)
        {
            return true;
        }
        if (result < 0)
        {
            spdlog::debug("audio transcoder encode receive failed {}", ffmpeg_error(result));
            return false;
        }
        if (state_->output_packet->size <= 0 || state_->output_packet->data == nullptr)
        {
            continue;
        }

        auto sample_count = state_->output_packet->duration;
        if (sample_count <= 0)
        {
            sample_count = av_get_audio_frame_duration(state_->encoder, state_->output_packet->size);
        }
        if (sample_count <= 0)
        {
            return false;
        }

        const auto pts_ns = state_->next_output_pts_ns;
        state_->next_output_pts_ns += av_rescale_q(sample_count, AVRational{1, state_->encoder->sample_rate}, AVRational{1, 1'000'000'000});
        output.push_back(media_frame{
            .track = state_->output_track,
            .dts_ns = pts_ns,
            .pts_ns = pts_ns,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(state_->output_packet->data,
                                                                         state_->output_packet->data + state_->output_packet->size),
        });
        spdlog::trace("audio transcoder packet encoded bytes {} samples {} pts_ns {}", state_->output_packet->size, sample_count, pts_ns);
    }
}

}    // namespace media_server
