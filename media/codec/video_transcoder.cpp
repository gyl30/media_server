#include "media/codec/video_transcoder.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>

namespace media_server
{
namespace
{

constexpr AVRational nanoseconds_time_base{1, 1'000'000'000};

std::string ffmpeg_error(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, buffer, sizeof(buffer)) != 0)
    {
        return std::to_string(error);
    }
    return buffer;
}

AVCodecID ffmpeg_codec(codec_id codec)
{
    return codec == codec_id::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

}    // namespace

struct video_transcoder::state
{
    AVCodecContext* decoder{};
    AVCodecContext* encoder{};
    SwsContext* scaler{};
    AVFrame* decoded_frame{};
    AVFrame* converted_frame{};
    AVPacket* input_packet{};
    AVPacket* output_packet{};
    codec_id input_codec{};
    std::optional<av1_encoding_parameters> av1;
    track_id output_track{};
    int width{};
    int height{};
    AVPixelFormat decoded_format{AV_PIX_FMT_NONE};
    AVPixelFormat encoder_format{AV_PIX_FMT_NONE};
    bool timeline_started{};
    bool input_ended{};
    bool flushed{};
};

video_transcoder::video_transcoder() = default;

video_transcoder::~video_transcoder() { shutdown(); }

bool video_transcoder::startup(const video_transcoder_config& config)
{
    shutdown();

    const bool annex_b_config = config.input_codec_config.size() >= 4 && config.input_codec_config[0] == 0 && config.input_codec_config[1] == 0 &&
        ((config.input_codec_config[2] == 1) || (config.input_codec_config[2] == 0 && config.input_codec_config[3] == 1));
    if ((config.input_codec != codec_id::h264 && config.input_codec != codec_id::h265) || config.output_codec != codec_id::av1 ||
        !annex_b_config || config.input_codec_config.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        (config.av1 && (config.av1->profile != 0 || config.av1->tier != 0)))
    {
        spdlog::error("video transcoder invalid config input {} output {} config_bytes {}",
                      to_string(config.input_codec),
                      to_string(config.output_codec),
                      config.input_codec_config.size());
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(ffmpeg_codec(config.input_codec));
    if (decoder == nullptr)
    {
        spdlog::error("video transcoder decoder not found codec {}", to_string(config.input_codec));
        return false;
    }

    state_ = std::make_unique<state>();
    state_->input_codec = config.input_codec;
    state_->av1 = config.av1;
    state_->decoder = avcodec_alloc_context3(decoder);
    if (state_->decoder == nullptr)
    {
        spdlog::error("video transcoder decoder context allocate failed");
        shutdown();
        return false;
    }

    state_->decoder->pkt_timebase = nanoseconds_time_base;
    state_->decoder->extradata = static_cast<std::uint8_t*>(av_mallocz(config.input_codec_config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (state_->decoder->extradata == nullptr)
    {
        spdlog::error("video transcoder decoder extradata allocate failed");
        shutdown();
        return false;
    }
    state_->decoder->extradata_size = static_cast<int>(config.input_codec_config.size());
    std::memcpy(state_->decoder->extradata, config.input_codec_config.data(), config.input_codec_config.size());

    const int open_result = avcodec_open2(state_->decoder, decoder, nullptr);
    if (open_result < 0)
    {
        spdlog::error("video transcoder decoder open failed codec {} error {}", to_string(config.input_codec), ffmpeg_error(open_result));
        shutdown();
        return false;
    }

    state_->decoded_frame = av_frame_alloc();
    state_->input_packet = av_packet_alloc();
    state_->output_packet = av_packet_alloc();
    if (state_->decoded_frame == nullptr || state_->input_packet == nullptr || state_->output_packet == nullptr)
    {
        spdlog::error("video transcoder buffer allocate failed");
        shutdown();
        return false;
    }

    spdlog::debug("video transcoder started input {} output av1 decoder {}", to_string(config.input_codec), decoder->name);
    return true;
}

void video_transcoder::shutdown()
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
    if (state_->converted_frame != nullptr)
    {
        av_frame_free(&state_->converted_frame);
    }
    if (state_->scaler != nullptr)
    {
        sws_freeContext(state_->scaler);
        state_->scaler = nullptr;
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

bool video_transcoder::transcode(const media_frame& input, std::vector<media_frame>& output)
{
    const bool annex_b_payload = input.payload && input.payload->size() >= 4 && (*input.payload)[0] == 0 && (*input.payload)[1] == 0 &&
        (((*input.payload)[2] == 1) || ((*input.payload)[2] == 0 && (*input.payload)[3] == 1));
    if (!state_ || state_->input_ended || !input.payload || input.payload->empty() || input.pts_ns == AV_NOPTS_VALUE ||
        input.dts_ns == AV_NOPTS_VALUE || input.payload->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !annex_b_payload || (state_->timeline_started && input.track != state_->output_track))
    {
        return false;
    }

    av_packet_unref(state_->input_packet);
    int result = av_new_packet(state_->input_packet, static_cast<int>(input.payload->size()));
    if (result < 0)
    {
        spdlog::error("video transcoder input packet allocate failed {}", ffmpeg_error(result));
        return false;
    }
    std::memcpy(state_->input_packet->data, input.payload->data(), input.payload->size());
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
        spdlog::debug("video transcoder decode packet failed codec {} error {}", to_string(state_->input_codec), ffmpeg_error(result));
        return false;
    }
    if (!state_->timeline_started)
    {
        state_->output_track = input.track;
        state_->timeline_started = true;
    }
    return receive_decoded(output, false);
}

bool video_transcoder::flush(std::vector<media_frame>& output)
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
        spdlog::error("video transcoder decoder flush failed {}", ffmpeg_error(result));
        return false;
    }
    if (result >= 0 && !receive_decoded(output, true))
    {
        return false;
    }

    if (state_->encoder != nullptr)
    {
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
            spdlog::error("video transcoder encoder flush failed {}", ffmpeg_error(result));
            return false;
        }
        if (result >= 0 && !receive_encoded(output, true))
        {
            return false;
        }
    }

    state_->flushed = true;
    return true;
}

bool video_transcoder::receive_decoded(std::vector<media_frame>& output, bool draining)
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
            spdlog::debug("video transcoder decode frame failed codec {} error {}", to_string(state_->input_codec), ffmpeg_error(result));
            return false;
        }
        if (state_->decoded_frame->pts == AV_NOPTS_VALUE || (!state_->encoder && !startup_encoder()) || !encode_decoded(output))
        {
            return false;
        }
    }
}

bool video_transcoder::startup_encoder()
{
    const AVCodec* encoder = avcodec_find_encoder_by_name("libaom-av1");
    if (encoder == nullptr)
    {
        spdlog::error("video transcoder av1 encoder not found");
        return false;
    }
    if (state_->decoded_frame->width <= 0 || state_->decoded_frame->height <= 0 || state_->decoded_frame->format < 0)
    {
        return false;
    }

    const void* formats{};
    int format_count{};
    int result = avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &format_count);
    if (result < 0 || formats == nullptr || format_count <= 0)
    {
        spdlog::error("video transcoder encoder pixel formats query failed {}", ffmpeg_error(result));
        return false;
    }

    const int width = state_->decoded_frame->width;
    const int height = state_->decoded_frame->height;
    const auto decoded_format = static_cast<AVPixelFormat>(state_->decoded_frame->format);
    AVPixelFormat encoder_format = AV_PIX_FMT_NONE;
    const auto* pixel_formats = static_cast<const AVPixelFormat*>(formats);
    if (state_->av1)
    {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(decoded_format);
        encoder_format = descriptor != nullptr && descriptor->comp[0].depth > 8 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
        if (std::find(pixel_formats, pixel_formats + format_count, encoder_format) == pixel_formats + format_count)
        {
            encoder_format = AV_PIX_FMT_YUV420P;
        }
        if (std::find(pixel_formats, pixel_formats + format_count, encoder_format) == pixel_formats + format_count)
        {
            spdlog::error("video transcoder av1 main profile pixel format not supported");
            return false;
        }
    }
    else
    {
        const auto direct = std::find(pixel_formats, pixel_formats + format_count, decoded_format);
        if (direct != pixel_formats + format_count)
        {
            encoder_format = decoded_format;
        }
        else
        {
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(decoded_format);
            for (int index = 0; index < format_count; ++index)
            {
                encoder_format = av_find_best_pix_fmt_of_2(encoder_format,
                                                           pixel_formats[index],
                                                           decoded_format,
                                                           descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0,
                                                           nullptr);
            }
            if (encoder_format == AV_PIX_FMT_NONE)
            {
                spdlog::error("video transcoder compatible pixel format not found");
                return false;
            }
        }
    }

    AVCodecContext* encoder_context = avcodec_alloc_context3(encoder);
    if (encoder_context == nullptr)
    {
        spdlog::error("video transcoder encoder context allocate failed");
        return false;
    }
    SwsContext* scaler{};
    AVFrame* converted_frame{};
    const auto cleanup = [&]() {
        if (converted_frame != nullptr)
        {
            av_frame_free(&converted_frame);
        }
        if (scaler != nullptr)
        {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        avcodec_free_context(&encoder_context);
    };

    encoder_context->width = width;
    encoder_context->height = height;
    encoder_context->pix_fmt = encoder_format;
    if (state_->av1)
    {
        encoder_context->profile = AV_PROFILE_AV1_MAIN;
    }
    encoder_context->framerate = state_->decoder->framerate;
    // libaom 的 presentation time 为 32 位，不能直接使用核心层的纳秒时间基。
    encoder_context->time_base = encoder_context->framerate.num > 0 && encoder_context->framerate.den > 0
        ? av_inv_q(encoder_context->framerate)
        : AVRational{1, 1'000};
    encoder_context->color_range = state_->decoded_frame->color_range;
    encoder_context->color_primaries = state_->decoded_frame->color_primaries;
    encoder_context->color_trc = state_->decoded_frame->color_trc;
    encoder_context->colorspace = state_->decoded_frame->colorspace;
    encoder_context->chroma_sample_location = state_->decoded_frame->chroma_location;

    AVDictionary* aom_parameters{};
    if (state_->av1)
    {
        const auto level_idx = std::to_string(state_->av1->level_idx);
        if (av_dict_set(&aom_parameters, "target-seq-level-idx", level_idx.c_str(), 0) < 0 ||
            av_dict_set(&aom_parameters, "set-tier-mask", "0", 0) < 0 ||
            av_dict_set(&aom_parameters, "strict-level-conformance", "1", 0) < 0)
        {
            av_dict_free(&aom_parameters);
            cleanup();
            spdlog::error("video transcoder av1 parameters allocate failed");
            return false;
        }
    }
    const bool options_failed = av_opt_set(encoder_context->priv_data, "usage", "realtime", 0) < 0 ||
        av_opt_set_int(encoder_context->priv_data, "cpu-used", 8, 0) < 0 ||
        av_opt_set_int(encoder_context->priv_data, "lag-in-frames", 0, 0) < 0 ||
        av_opt_set_int(encoder_context->priv_data, "crf", 32, 0) < 0 ||
        (aom_parameters != nullptr && av_opt_set_dict_val(encoder_context->priv_data, "aom-params", aom_parameters, 0) < 0);
    av_dict_free(&aom_parameters);
    if (options_failed)
    {
        cleanup();
        spdlog::error("video transcoder encoder options failed");
        return false;
    }

    result = avcodec_open2(encoder_context, encoder, nullptr);
    if (result < 0)
    {
        cleanup();
        spdlog::error("video transcoder encoder open failed encoder {} error {}", encoder->name, ffmpeg_error(result));
        return false;
    }

    if (encoder_format != decoded_format)
    {
        AVPixelFormat scaler_input_format = decoded_format;
        switch (scaler_input_format)
        {
        case AV_PIX_FMT_YUVJ420P:
            scaler_input_format = AV_PIX_FMT_YUV420P;
            break;
        case AV_PIX_FMT_YUVJ422P:
            scaler_input_format = AV_PIX_FMT_YUV422P;
            break;
        case AV_PIX_FMT_YUVJ444P:
            scaler_input_format = AV_PIX_FMT_YUV444P;
            break;
        default:
            break;
        }
        const int full_range = state_->decoded_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        scaler = sws_alloc_context();
        if (scaler != nullptr &&
            (av_opt_set_int(scaler, "srcw", width, 0) < 0 || av_opt_set_int(scaler, "srch", height, 0) < 0 ||
             av_opt_set_int(scaler, "dstw", width, 0) < 0 || av_opt_set_int(scaler, "dsth", height, 0) < 0 ||
             av_opt_set_int(scaler, "src_format", scaler_input_format, 0) < 0 || av_opt_set_int(scaler, "dst_format", encoder_format, 0) < 0 ||
             av_opt_set_int(scaler, "sws_flags", SWS_BILINEAR, 0) < 0 || av_opt_set_int(scaler, "src_range", full_range, 0) < 0 ||
             av_opt_set_int(scaler, "dst_range", full_range, 0) < 0 || sws_init_context(scaler, nullptr, nullptr) < 0))
        {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        converted_frame = av_frame_alloc();
        if (scaler == nullptr || converted_frame == nullptr)
        {
            cleanup();
            spdlog::error("video transcoder pixel converter allocate failed");
            return false;
        }
        int sws_colorspace = SWS_CS_DEFAULT;
        switch (state_->decoded_frame->colorspace)
        {
        case AVCOL_SPC_BT709:
            sws_colorspace = SWS_CS_ITU709;
            break;
        case AVCOL_SPC_FCC:
            sws_colorspace = SWS_CS_FCC;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            sws_colorspace = SWS_CS_SMPTE170M;
            break;
        case AVCOL_SPC_SMPTE240M:
            sws_colorspace = SWS_CS_SMPTE240M;
            break;
        case AVCOL_SPC_BT2020_NCL:
            sws_colorspace = SWS_CS_BT2020;
            break;
        default:
            break;
        }
        const int colorspace_result = sws_setColorspaceDetails(scaler,
                                                               sws_getCoefficients(sws_colorspace),
                                                               full_range,
                                                               sws_getCoefficients(sws_colorspace),
                                                               full_range,
                                                               0,
                                                               1 << 16,
                                                               1 << 16);
        if (colorspace_result < 0)
        {
            cleanup();
            spdlog::error("video transcoder pixel converter colorspace failed {}", ffmpeg_error(colorspace_result));
            return false;
        }
        converted_frame->format = encoder_format;
        converted_frame->width = width;
        converted_frame->height = height;
        result = av_frame_get_buffer(converted_frame, 32);
        if (result < 0)
        {
            cleanup();
            spdlog::error("video transcoder converted frame allocate failed {}", ffmpeg_error(result));
            return false;
        }
    }

    state_->width = width;
    state_->height = height;
    state_->decoded_format = decoded_format;
    state_->encoder_format = encoder_format;
    state_->encoder = encoder_context;
    state_->scaler = scaler;
    state_->converted_frame = converted_frame;

    spdlog::debug("video transcoder encoder started name {} width {} height {} input_format {} output_format {}",
                  encoder->name,
                  state_->width,
                  state_->height,
                  av_get_pix_fmt_name(state_->decoded_format),
                  av_get_pix_fmt_name(state_->encoder_format));
    return true;
}

bool video_transcoder::encode_decoded(std::vector<media_frame>& output)
{
    if (state_->decoded_frame->width != state_->width || state_->decoded_frame->height != state_->height ||
        state_->decoded_frame->format != state_->decoded_format)
    {
        spdlog::error("video transcoder decoded format changed");
        return false;
    }

    AVFrame* frame = state_->decoded_frame;
    if (state_->scaler != nullptr)
    {
        int result = av_frame_make_writable(state_->converted_frame);
        if (result < 0)
        {
            return false;
        }
        result = sws_scale(state_->scaler,
                           state_->decoded_frame->data,
                           state_->decoded_frame->linesize,
                           0,
                           state_->height,
                           state_->converted_frame->data,
                           state_->converted_frame->linesize);
        if (result != state_->height)
        {
            spdlog::error("video transcoder pixel conversion failed rows {}", result);
            return false;
        }
        while (state_->converted_frame->nb_side_data > 0)
        {
            av_frame_remove_side_data(state_->converted_frame, state_->converted_frame->side_data[0]->type);
        }
        av_dict_free(&state_->converted_frame->metadata);
        result = av_frame_copy_props(state_->converted_frame, state_->decoded_frame);
        if (result < 0)
        {
            spdlog::error("video transcoder frame properties copy failed {}", ffmpeg_error(result));
            return false;
        }
        frame = state_->converted_frame;
    }

    frame->pts = av_rescale_q(frame->pts, nanoseconds_time_base, state_->encoder->time_base);
    if (frame->duration > 0)
    {
        frame->duration = av_rescale_q(frame->duration, nanoseconds_time_base, state_->encoder->time_base);
    }

    int result = avcodec_send_frame(state_->encoder, frame);
    if (result == AVERROR(EAGAIN))
    {
        if (!receive_encoded(output, false))
        {
            return false;
        }
        result = avcodec_send_frame(state_->encoder, frame);
    }
    if (result < 0)
    {
        spdlog::debug("video transcoder encode frame failed {}", ffmpeg_error(result));
        return false;
    }
    return receive_encoded(output, false);
}

bool video_transcoder::receive_encoded(std::vector<media_frame>& output, bool draining)
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
            spdlog::debug("video transcoder encode packet failed {}", ffmpeg_error(result));
            return false;
        }
        if (state_->output_packet->data == nullptr || state_->output_packet->size <= 0 || state_->output_packet->pts == AV_NOPTS_VALUE ||
            state_->output_packet->dts == AV_NOPTS_VALUE)
        {
            return false;
        }

        output.push_back(media_frame{
            .track = state_->output_track,
            .dts_ns = av_rescale_q(state_->output_packet->dts, state_->encoder->time_base, nanoseconds_time_base),
            .pts_ns = av_rescale_q(state_->output_packet->pts, state_->encoder->time_base, nanoseconds_time_base),
            .key_frame = (state_->output_packet->flags & AV_PKT_FLAG_KEY) != 0,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(
                state_->output_packet->data, state_->output_packet->data + state_->output_packet->size),
        });
        spdlog::trace("video transcoder packet encoded bytes {} pts {} dts {} key {}",
                      state_->output_packet->size,
                      state_->output_packet->pts,
                      state_->output_packet->dts,
                      (state_->output_packet->flags & AV_PKT_FLAG_KEY) != 0);
    }
}

}    // namespace media_server
