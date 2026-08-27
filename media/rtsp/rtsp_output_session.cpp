#include <array>
#include <memory>
#include <sstream>
#include <utility>

#include <boost/system/error_code.hpp>
#include <optional>
#include <algorithm>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_uri.h"
#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/rtsp/rtsp_output_tcp_session.h"

extern "C"
{
#include "aom-av1.h"
#include "rtsp-muxer.h"
#include "rtp-profile.h"
#include "rtsp-server.h"
#include "rtsp-header-transport.h"
}

namespace media_server
{

namespace
{
constexpr av1_encoding_parameters rtsp_av1_parameters{
    .profile = 0,
    .level_idx = 13,
    .tier = 0,
};

struct prepared_rtsp_output_track
{
    rtsp_output_track_description description;
    std::unique_ptr<video_transcoder> transcoder;
};

std::optional<prepared_rtsp_output_track> prepare_rtsp_output_track(const media_track& track, output_video_codec video_codec, int& next_payload_type)
{
    prepared_rtsp_output_track prepared;
    prepared.description.track = track;
    if (track.kind == media_kind::video && video_codec == output_video_codec::av1)
    {
        aom_av1_t av1{};
        av1.marker = 1;
        av1.version = 1;
        av1.seq_profile = rtsp_av1_parameters.profile;
        av1.seq_level_idx_0 = rtsp_av1_parameters.level_idx;
        av1.seq_tier_0 = rtsp_av1_parameters.tier;
        av1.chroma_subsampling_x = 1;
        av1.chroma_subsampling_y = 1;
        std::array<std::uint8_t, 4> config{};
        if (aom_av1_codec_configuration_record_save(&av1, config.data(), config.size()) != static_cast<int>(config.size()))
        {
            return std::nullopt;
        }
        prepared.description.extra.assign(config.begin(), config.end());
        prepared.description.encoding = "AV1";
        prepared.description.rtp_codec = RTP_PAYLOAD_AV1;
        prepared.description.frequency = 90'000;
        prepared.description.payload_type = next_payload_type++;

        auto transcoder = std::make_unique<video_transcoder>();
        if (!transcoder->startup(video_transcoder_config{
                .input_codec = track.codec,
                .output_codec = codec_id::av1,
                .input_codec_config = track.codec_config,
                .av1 = rtsp_av1_parameters,
            }))
        {
            return std::nullopt;
        }
        prepared.transcoder = std::move(transcoder);
    }
    else if (track.codec == codec_id::h264)
    {
        prepared.description.extra = h264_annex_b_to_avcc(track.codec_config);
        if (prepared.description.extra.empty())
        {
            return std::nullopt;
        }
        prepared.description.encoding = "H264";
        prepared.description.rtp_codec = RTP_PAYLOAD_H264;
        prepared.description.frequency = 90'000;
        prepared.description.payload_type = next_payload_type++;
    }
    else if (track.codec == codec_id::h265)
    {
        prepared.description.extra = h265_annex_b_to_hvcc(track.codec_config);
        if (prepared.description.extra.empty())
        {
            return std::nullopt;
        }
        prepared.description.encoding = "H265";
        prepared.description.rtp_codec = RTP_PAYLOAD_H265;
        prepared.description.frequency = 90'000;
        prepared.description.payload_type = next_payload_type++;
    }
    else if (track.codec == codec_id::aac)
    {
        prepared.description.extra = track.codec_config;
        if (prepared.description.extra.empty() || track.clock_rate == 0)
        {
            return std::nullopt;
        }
        prepared.description.encoding = "MPEG4-GENERIC";
        prepared.description.rtp_codec = RTP_PAYLOAD_MP4A;
        prepared.description.frequency = static_cast<int>(track.clock_rate);
        prepared.description.payload_type = next_payload_type++;
    }
    else if (track.codec == codec_id::opus)
    {
        prepared.description.encoding = "opus";
        prepared.description.rtp_codec = RTP_PAYLOAD_OPUS;
        prepared.description.frequency = 48'000;
        prepared.description.payload_type = next_payload_type++;
    }
    else if (track.codec == codec_id::g711a)
    {
        prepared.description.encoding = "PCMA";
        prepared.description.rtp_codec = RTP_PAYLOAD_PCMA;
        prepared.description.frequency = 8'000;
        prepared.description.payload_type = RTP_PAYLOAD_PCMA;
    }
    else if (track.codec == codec_id::g711u)
    {
        prepared.description.encoding = "PCMU";
        prepared.description.rtp_codec = RTP_PAYLOAD_PCMU;
        prepared.description.frequency = 8'000;
        prepared.description.payload_type = RTP_PAYLOAD_PCMU;
    }
    return prepared;
}
}    // namespace

rtsp_output_session::rtsp_output_session(boost::asio::any_io_executor executor,
                                         const config& config,
                                         std::string local_address,
                                         std::function<void(std::span<const std::uint8_t>)> write)
    : executor_(std::move(executor)), video_config_(config.rtsp_video), local_address_(std::move(local_address)), write_(std::move(write))
{
}

void rtsp_output_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (!tcp_session_)
    {
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
        return;
    }
    tcp_session_->on_interleaved(channel, data);
}

void rtsp_output_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    if (tcp_session_)
    {
        tcp_session_->safe_shutdown();
        tcp_session_.reset();
    }
    tracks_.clear();
    if (video_transcoder_)
    {
        video_transcoder_->shutdown();
        video_transcoder_.reset();
    }
    video_track_id_ = 0;
    stream_.reset();
    write_ = {};
    error_handle_ = {};
}

int rtsp_output_session::on_describe(rtsp_server_t* server, std::string_view uri)
{
    if (tcp_session_)
    {
        return rtsp_server_reply_describe(server, 455, "");
    }
    const auto prepare_result = prepare_stream(uri);
    if (prepare_result != 0)
    {
        return rtsp_server_reply_describe(server, prepare_result, "");
    }

    auto muxer =
        std::unique_ptr<rtsp_muxer_t, void (*)(rtsp_muxer_t*)>(rtsp_muxer_create(
                                                                   +[](void*, int, const void*, int, std::uint32_t, int) { return 0; }, nullptr),
                                                               [](rtsp_muxer_t* value) { rtsp_muxer_destroy(value); });
    if (!muxer)
    {
        tracks_.clear();
        if (video_transcoder_)
        {
            video_transcoder_->shutdown();
            video_transcoder_.reset();
        }
        video_track_id_ = 0;
        stream_.reset();
        return rtsp_server_reply_describe(server, 500, "");
    }

    std::ostringstream media_sdp;
    for (const auto& description : tracks_)
    {
        const auto payload_index = rtsp_muxer_add_payload(muxer.get(),
                                                          "RTP/AVP",
                                                          description.frequency,
                                                          description.payload_type,
                                                          description.encoding.c_str(),
                                                          0,
                                                          0,
                                                          0,
                                                          description.extra.data(),
                                                          static_cast<int>(description.extra.size()));
        if (payload_index < 0 ||
            rtsp_muxer_add_media(
                muxer.get(), payload_index, description.rtp_codec, description.extra.data(), static_cast<int>(description.extra.size())) < 0)
        {
            tracks_.clear();
            if (video_transcoder_)
            {
                video_transcoder_->shutdown();
                video_transcoder_.reset();
            }
            video_track_id_ = 0;
            stream_.reset();
            return rtsp_server_reply_describe(server, 415, "");
        }

        std::uint16_t sequence{};
        std::uint32_t timestamp{};
        const char* media_text{};
        int media_text_size{};
        if (rtsp_muxer_getinfo(muxer.get(), payload_index, &sequence, &timestamp, &media_text, &media_text_size) != 0)
        {
            tracks_.clear();
            if (video_transcoder_)
            {
                video_transcoder_->shutdown();
                video_transcoder_.reset();
            }
            video_track_id_ = 0;
            stream_.reset();
            return rtsp_server_reply_describe(server, 415, "");
        }
        media_sdp.write(media_text, media_text_size);
        media_sdp << "a=control:" << description.control << "\r\n";
    }

    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 1 1 IN IP4 " << local_address_ << "\r\n"
        << "s=media_server\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << media_sdp.str();

    spdlog::info("rtsp output describe {}", stream_name_);
    return rtsp_server_reply_describe(server, 200, sdp.str().c_str());
}

int rtsp_output_session::on_setup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (tcp_session_)
    {
        return tcp_session_->on_setup(server, uri, session, transports, count);
    }
    if (!stream_)
    {
        const auto path = rtsp_path_from_uri(uri);
        const auto separator = path.rfind('/');
        if (separator == std::string::npos)
        {
            return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
        }
        const auto prepare_result = prepare_stream(path.substr(0, separator));
        if (prepare_result != 0)
        {
            return rtsp_server_reply_setup(server, prepare_result, nullptr, nullptr);
        }
    }
    const auto current_stream = registry::instance().find(stream_name_);
    if (!stream_ || !current_stream)
    {
        return rtsp_server_reply_setup(server, 503, nullptr, nullptr);
    }
    if (current_stream.get() != stream_.get() || !description_current())
    {
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }
    if (transports == nullptr || count == 0)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    const rtsp_header_transport_t* selected = nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (transports[index].transport == RTSP_TRANSPORT_RTP_TCP && transports[index].multicast == 0 &&
            (transports[index].mode == 0 || transports[index].mode == RTSP_TRANSPORT_PLAY))
        {
            selected = &transports[index];
            break;
        }
    }
    if (selected == nullptr)
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    auto child =
        std::make_shared<rtsp_output_tcp_session>(executor_, stream_, stream_name_, tracks_, video_track_id_, write_);
    child->set_error_handle(error_handle_);
    const auto result = child->startup(server, uri, session, transports, count, video_transcoder_);
    if (!child->closed_)
    {
        tcp_session_ = std::move(child);
    }
    return result;
}

int rtsp_output_session::on_play(rtsp_server_t* server,
                                 std::string_view uri,
                                 std::string_view session,
                                 const std::int64_t* npt,
                                 const double*)
{
    if (tcp_session_)
    {
        return tcp_session_->on_play(server, uri, session, npt);
    }
    if (rtsp_path_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
    }
    return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
}

int rtsp_output_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (tcp_session_)
    {
        return tcp_session_->on_teardown(server, session);
    }
    return rtsp_server_reply_teardown(server, 454);
}

int rtsp_output_session::on_get_parameter(rtsp_server_t* server, std::string_view, std::string_view, const void*, int)
{
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

int rtsp_output_session::prepare_stream(std::string_view uri)
{
    tracks_.clear();
    if (video_transcoder_)
    {
        video_transcoder_->shutdown();
        video_transcoder_.reset();
    }
    video_track_id_ = 0;
    stream_.reset();

    stream_name_ = rtsp_path_from_uri(uri);
    auto stream = registry::instance().find(stream_name_);
    if (!stream)
    {
        return 404;
    }
    const auto snapshot = stream->tracks();

    std::vector<rtsp_output_track_description> descriptions;
    std::unique_ptr<video_transcoder> prepared_transcoder;
    track_id video_track_id{};
    int next_payload_type = 96;
    for (const auto& track : snapshot)
    {
        if (!rtsp_output_track_supported(track))
        {
            continue;
        }

        auto prepared = prepare_rtsp_output_track(track, video_config_.codec, next_payload_type);
        if (!prepared)
        {
            if (prepared_transcoder)
            {
                prepared_transcoder->shutdown();
            }
            return 415;
        }

        if (prepared->transcoder)
        {
            if (prepared_transcoder)
            {
                prepared_transcoder->shutdown();
            }
            video_track_id = track.id;
            prepared_transcoder = std::move(prepared->transcoder);
        }
        prepared->description.control = "trackID=" + std::to_string(track.id);
        descriptions.emplace_back(std::move(prepared->description));
    }

    if (descriptions.empty())
    {
        return 415;
    }

    stream_ = std::move(stream);
    tracks_ = std::move(descriptions);
    video_transcoder_ = std::move(prepared_transcoder);
    video_track_id_ = video_track_id;
    return 0;
}

bool rtsp_output_session::description_current() const
{
    if (!stream_)
    {
        return false;
    }

    const auto current = stream_->tracks();
    std::size_t supported_count = 0;
    for (const auto& track : current)
    {
        if (!rtsp_output_track_supported(track))
        {
            continue;
        }
        ++supported_count;
        const auto iterator =
            std::ranges::find_if(tracks_, [id = track.id](const rtsp_output_track_description& value) { return value.track.id == id; });
        if (iterator == tracks_.end() || iterator->track.config_version != track.config_version)
        {
            return false;
        }
    }
    return supported_count == tracks_.size();
}

}    // namespace media_server
