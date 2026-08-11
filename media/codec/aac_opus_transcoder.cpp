#include "media/codec/aac_opus_transcoder.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace media_server
{
namespace
{

constexpr int opus_sample_rate = 48'000;
constexpr std::int64_t opus_bitrate_per_channel = 64'000;
constexpr int default_opus_frame_samples = 960;

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
    if (frame_size != frame.size())
    {
        return std::nullopt;
    }

    if ((frame[6] & 0x03U) != 0U)
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

aac_opus_transcoder::~aac_opus_transcoder() { shutdown(); }

bool aac_opus_transcoder::startup(std::span<const std::uint8_t> audio_specific_config, int output_channel_count)
{
    shutdown();

    if (output_channel_count != 1 && output_channel_count != 2)
    {
        spdlog::error("webrtc opus invalid output channel count {}", output_channel_count);
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (decoder == nullptr)
    {
        spdlog::error("webrtc aac decoder not found");
        return false;
    }

    if (audio_specific_config.empty() || audio_specific_config.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        spdlog::error("webrtc aac audio specific config missing");
        return false;
    }

    decoder_ = avcodec_alloc_context3(decoder);
    if (decoder_ == nullptr)
    {
        spdlog::error("webrtc aac decoder context allocate failed");
        shutdown();
        return false;
    }

    decoder_->extradata = static_cast<std::uint8_t*>(av_mallocz(audio_specific_config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (decoder_->extradata == nullptr)
    {
        spdlog::error("webrtc aac decoder extradata allocate failed");
        shutdown();
        return false;
    }
    decoder_->extradata_size = static_cast<int>(audio_specific_config.size());
    std::memcpy(decoder_->extradata, audio_specific_config.data(), audio_specific_config.size());

    int result = avcodec_open2(decoder_, decoder, nullptr);
    if (result < 0)
    {
        spdlog::error("webrtc aac decoder open failed {}", ffmpeg_error(result));
        shutdown();
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
    if (encoder == nullptr)
    {
        encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (encoder == nullptr)
    {
        spdlog::error("webrtc opus encoder not found");
        shutdown();
        return false;
    }

    const void* sample_formats{};
    result = avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &sample_formats, nullptr);
    if (result < 0 || sample_formats == nullptr)
    {
        spdlog::error("webrtc opus sample formats query failed {}", ffmpeg_error(result));
        shutdown();
        return false;
    }

    encoder_ = avcodec_alloc_context3(encoder);
    if (encoder_ == nullptr)
    {
        spdlog::error("webrtc opus encoder context allocate failed");
        shutdown();
        return false;
    }

    av_channel_layout_default(&encoder_->ch_layout, output_channel_count);
    encoder_->sample_rate = opus_sample_rate;
    encoder_->sample_fmt = static_cast<const AVSampleFormat*>(sample_formats)[0];
    encoder_->bit_rate = opus_bitrate_per_channel * output_channel_count;
    encoder_->time_base = AVRational{1, opus_sample_rate};
    encoder_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    result = avcodec_open2(encoder_, encoder, nullptr);
    if (result < 0)
    {
        spdlog::error("webrtc opus encoder open failed {}", ffmpeg_error(result));
        shutdown();
        return false;
    }

    encoder_frame_samples_ = encoder_->frame_size > 0 ? encoder_->frame_size : default_opus_frame_samples;
    fifo_ = av_audio_fifo_alloc(encoder_->sample_fmt, encoder_->ch_layout.nb_channels, encoder_frame_samples_);
    decoded_frame_ = av_frame_alloc();
    encoded_frame_ = av_frame_alloc();
    input_packet_ = av_packet_alloc();
    output_packet_ = av_packet_alloc();
    if (fifo_ == nullptr || decoded_frame_ == nullptr || encoded_frame_ == nullptr || input_packet_ == nullptr || output_packet_ == nullptr)
    {
        spdlog::error("webrtc audio transcoder buffer allocate failed");
        shutdown();
        return false;
    }

    encoded_frame_->format = encoder_->sample_fmt;
    encoded_frame_->sample_rate = encoder_->sample_rate;
    encoded_frame_->nb_samples = encoder_frame_samples_;
    if (av_channel_layout_copy(&encoded_frame_->ch_layout, &encoder_->ch_layout) < 0)
    {
        spdlog::error("webrtc opus frame channel layout copy failed");
        shutdown();
        return false;
    }

    result = av_frame_get_buffer(encoded_frame_, 0);
    if (result < 0)
    {
        spdlog::error("webrtc opus frame buffer allocate failed {}", ffmpeg_error(result));
        shutdown();
        return false;
    }

    spdlog::debug("webrtc audio transcoder started encoder {} sample_rate {} channels {} frame_samples {} bitrate {}",
                  encoder->name,
                  encoder_->sample_rate,
                  encoder_->ch_layout.nb_channels,
                  encoder_frame_samples_,
                  encoder_->bit_rate);
    return true;
}

bool aac_opus_transcoder::transcode(std::span<const std::uint8_t> adts_frame, std::vector<opus_audio_packet>& packets)
{
    if (decoder_ == nullptr || encoder_ == nullptr || fifo_ == nullptr || decoded_frame_ == nullptr || input_packet_ == nullptr ||
        output_packet_ == nullptr || adts_frame.empty() || adts_frame.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    const auto payload = adts_payload(adts_frame);
    if (!payload || payload->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        spdlog::debug("webrtc invalid aac adts frame size {}", adts_frame.size());
        return false;
    }

    av_packet_unref(input_packet_);
    int result = av_new_packet(input_packet_, static_cast<int>(payload->size()));
    if (result < 0)
    {
        spdlog::error("webrtc aac packet allocate failed {}", ffmpeg_error(result));
        return false;
    }
    std::memcpy(input_packet_->data, payload->data(), payload->size());

    result = avcodec_send_packet(decoder_, input_packet_);
    if (result < 0)
    {
        spdlog::debug("webrtc aac decode send failed {}", ffmpeg_error(result));
        return false;
    }

    while (true)
    {
        av_frame_unref(decoded_frame_);
        result = avcodec_receive_frame(decoder_, decoded_frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        {
            break;
        }
        if (result < 0)
        {
            spdlog::debug("webrtc aac decode receive failed {}", ffmpeg_error(result));
            return false;
        }

        if (resampler_ == nullptr && !configure_resampler(*decoded_frame_))
        {
            return false;
        }
        if (!resample(*decoded_frame_))
        {
            return false;
        }
    }

    return encode_available(packets);
}

void aac_opus_transcoder::shutdown()
{
    drain_encoder();

    if (input_packet_ != nullptr)
    {
        av_packet_free(&input_packet_);
    }
    if (output_packet_ != nullptr)
    {
        av_packet_free(&output_packet_);
    }
    if (decoded_frame_ != nullptr)
    {
        av_frame_free(&decoded_frame_);
    }
    if (encoded_frame_ != nullptr)
    {
        av_frame_free(&encoded_frame_);
    }
    if (fifo_ != nullptr)
    {
        av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
    }
    if (resampler_ != nullptr)
    {
        swr_free(&resampler_);
    }
    if (decoder_ != nullptr)
    {
        avcodec_free_context(&decoder_);
    }
    if (encoder_ != nullptr)
    {
        avcodec_free_context(&encoder_);
    }
    next_pts_ = 0;
    encoder_frame_samples_ = 0;
}

void aac_opus_transcoder::drain_encoder()
{
    if (encoder_ == nullptr || output_packet_ == nullptr)
    {
        return;
    }

    const int send_result = avcodec_send_frame(encoder_, nullptr);
    if (send_result < 0 && send_result != AVERROR_EOF)
    {
        return;
    }

    while (true)
    {
        av_packet_unref(output_packet_);
        const int receive_result = avcodec_receive_packet(encoder_, output_packet_);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF)
        {
            return;
        }
        if (receive_result < 0)
        {
            return;
        }
    }
}

bool aac_opus_transcoder::configure_resampler(const AVFrame& frame)
{
    if (encoder_ == nullptr || frame.sample_rate <= 0 || frame.ch_layout.nb_channels <= 0)
    {
        return false;
    }

    const auto input_format = static_cast<AVSampleFormat>(frame.format);
    const int result = swr_alloc_set_opts2(&resampler_,
                                           &encoder_->ch_layout,
                                           encoder_->sample_fmt,
                                           encoder_->sample_rate,
                                           &frame.ch_layout,
                                           input_format,
                                           frame.sample_rate,
                                           0,
                                           nullptr);
    if (result < 0 || resampler_ == nullptr)
    {
        spdlog::error("webrtc audio resampler allocate failed {}", ffmpeg_error(result));
        return false;
    }

    const int init_result = swr_init(resampler_);
    if (init_result < 0)
    {
        spdlog::error("webrtc audio resampler init failed {}", ffmpeg_error(init_result));
        swr_free(&resampler_);
        return false;
    }

    spdlog::debug("webrtc audio resampler started input_rate {} input_channels {} output_rate {} output_channels {}",
                  frame.sample_rate,
                  frame.ch_layout.nb_channels,
                  encoder_->sample_rate,
                  encoder_->ch_layout.nb_channels);
    return true;
}

bool aac_opus_transcoder::resample(const AVFrame& frame)
{
    if (resampler_ == nullptr || encoder_ == nullptr || fifo_ == nullptr || frame.sample_rate <= 0)
    {
        return false;
    }

    const auto delayed_samples = swr_get_delay(resampler_, frame.sample_rate);
    const auto output_capacity_value = av_rescale_rnd(delayed_samples + frame.nb_samples, encoder_->sample_rate, frame.sample_rate, AV_ROUND_UP);
    if (output_capacity_value <= 0 || output_capacity_value > std::numeric_limits<int>::max())
    {
        return false;
    }

    AVFrame* converted = av_frame_alloc();
    if (converted == nullptr)
    {
        return false;
    }

    converted->format = encoder_->sample_fmt;
    converted->sample_rate = encoder_->sample_rate;
    converted->nb_samples = static_cast<int>(output_capacity_value);
    if (av_channel_layout_copy(&converted->ch_layout, &encoder_->ch_layout) < 0)
    {
        av_frame_free(&converted);
        return false;
    }

    int result = av_frame_get_buffer(converted, 0);
    if (result < 0)
    {
        spdlog::error("webrtc audio resample frame allocate failed {}", ffmpeg_error(result));
        av_frame_free(&converted);
        return false;
    }

    std::vector<const std::uint8_t*> input_data(static_cast<std::size_t>(frame.ch_layout.nb_channels));
    for (std::size_t index = 0; index < input_data.size(); ++index)
    {
        input_data[index] = frame.extended_data[index];
    }

    result = swr_convert(resampler_, converted->extended_data, converted->nb_samples, input_data.data(), frame.nb_samples);
    if (result < 0)
    {
        spdlog::debug("webrtc audio resample failed {}", ffmpeg_error(result));
        av_frame_free(&converted);
        return false;
    }

    if (result > 0)
    {
        const int written = av_audio_fifo_write(fifo_, reinterpret_cast<void**>(converted->extended_data), result);
        if (written != result)
        {
            spdlog::error("webrtc audio fifo write failed expected {} actual {}", result, written);
            av_frame_free(&converted);
            return false;
        }
    }

    av_frame_free(&converted);
    return true;
}

bool aac_opus_transcoder::encode_available(std::vector<opus_audio_packet>& packets)
{
    if (fifo_ == nullptr || encoder_ == nullptr || encoded_frame_ == nullptr || encoder_frame_samples_ <= 0)
    {
        return false;
    }

    while (av_audio_fifo_size(fifo_) >= encoder_frame_samples_)
    {
        int result = av_frame_make_writable(encoded_frame_);
        if (result < 0)
        {
            spdlog::error("webrtc opus frame writable failed {}", ffmpeg_error(result));
            return false;
        }

        const int read = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(encoded_frame_->extended_data), encoder_frame_samples_);
        if (read != encoder_frame_samples_)
        {
            spdlog::error("webrtc audio fifo read failed expected {} actual {}", encoder_frame_samples_, read);
            return false;
        }

        encoded_frame_->nb_samples = encoder_frame_samples_;
        encoded_frame_->pts = next_pts_;
        next_pts_ += encoder_frame_samples_;

        result = avcodec_send_frame(encoder_, encoded_frame_);
        if (result < 0)
        {
            spdlog::debug("webrtc opus encode send failed {}", ffmpeg_error(result));
            return false;
        }
        if (!receive_encoded(packets))
        {
            return false;
        }
    }
    return true;
}

bool aac_opus_transcoder::receive_encoded(std::vector<opus_audio_packet>& packets)
{
    while (true)
    {
        av_packet_unref(output_packet_);
        const int result = avcodec_receive_packet(encoder_, output_packet_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        {
            return true;
        }
        if (result < 0)
        {
            spdlog::debug("webrtc opus encode receive failed {}", ffmpeg_error(result));
            return false;
        }
        if (output_packet_->size <= 0 || output_packet_->data == nullptr)
        {
            continue;
        }

        const auto sample_count = output_packet_->duration > 0 ? output_packet_->duration : static_cast<std::int64_t>(encoder_frame_samples_);
        if (sample_count <= 0 || sample_count > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }

        packets.push_back(opus_audio_packet{
            .payload = std::vector<std::uint8_t>(output_packet_->data, output_packet_->data + output_packet_->size),
            .sample_count = static_cast<std::uint32_t>(sample_count),
        });
        spdlog::trace("webrtc opus packet encoded bytes {} samples {}", output_packet_->size, sample_count);
    }
}

}    // namespace media_server
