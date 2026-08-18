#include "media/rtsp/rtsp_output_session.h"

#include "media/codec/codec_utils.h"
#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_tcp_session.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "aom-av1.h"
#include "rtp-profile.h"
#include "rtsp-header-transport.h"
#include "rtsp-muxer.h"
#include "rtsp-server.h"
}

#include <algorithm>
#include <array>
#include <boost/url/parse.hpp>
#include <charconv>
#include <memory>
#include <sstream>
#include <utility>

namespace media_server
{

namespace
{
constexpr av1_encoding_parameters rtsp_av1_parameters{
    .profile = 0,
    .level_idx = 13,
    .tier = 0,
};

bool supported_track(const media_track& track)
{
    return (track.kind == media_kind::video && (track.codec == codec_id::h264 || track.codec == codec_id::h265)) ||
           (track.kind == media_kind::audio &&
            (track.codec == codec_id::aac ||
             (track.codec == codec_id::opus && track.clock_rate == 48'000 &&
              (track.channel_count == 1 || track.channel_count == 2) && track.codec_config.empty()) ||
             ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) && track.clock_rate == 8'000 && track.channel_count == 1 &&
              track.codec_config.empty())));
}
}    // namespace

rtsp_output_session::rtsp_output_session(std::weak_ptr<rtsp_server_connection> connection, stream_registry& registry, output_video_config video)
    : connection_(std::move(connection)), registry_(registry), video_config_(video)
{
}

void rtsp_output_session::startup(std::vector<std::uint8_t> initial_data)
{
    const auto connection = connection_.lock();
    if (closed_ || !connection)
    {
        return;
    }

    const auto self = shared_from_this();
    auto handler = std::make_shared<rtsp_server_connection_handler>();
    handler->on_read = [self](std::span<const std::uint8_t> data) { return self->on_read(data); };
    handler->on_shutdown = [self]() { self->safe_shutdown(); };
    handler->on_describe = [self](rtsp_server_t* server, const char* uri) { return self->on_describe(server, uri); };
    handler->on_setup = [self](rtsp_server_t* server,
                               const char* uri,
                               const char* session,
                               const rtsp_header_transport_t transports[],
                               std::size_t count)
    { return self->on_setup(server, uri, session, transports, count); };
    handler->on_play = [self](rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double*)
    { return self->on_play(server, uri, session, npt); };
    handler->on_teardown = [self](rtsp_server_t* server, const char*, const char* session) { return self->on_teardown(server, session); };
    handler->on_get_parameter = [](rtsp_server_t* server, const char*, const char*, const void*, int)
    { return rtsp_server_reply_get_parameter(server, 200, nullptr, 0); };

    if (!connection->startup(std::move(handler), std::move(initial_data)))
    {
        connection->shutdown();
    }
}

void rtsp_output_session::shutdown()
{
    if (const auto connection = connection_.lock())
    {
        connection->shutdown();
    }
}

std::size_t rtsp_output_session::on_read(std::span<const std::uint8_t> data)
{
    const auto connection = connection_.lock();
    return connection ? connection->input(data) : data.size();
}

void rtsp_output_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    tracks_.clear();
    video_transcoder_.reset();
    video_track_id_ = 0;
    stream_.reset();
}

int rtsp_output_session::on_describe(rtsp_server_t* server, std::string_view uri)
{
    const auto prepare_result = prepare_stream(uri);
    if (prepare_result != 0)
    {
        return rtsp_server_reply_describe(server, prepare_result, "");
    }

    auto muxer = std::unique_ptr<rtsp_muxer_t, void (*)(rtsp_muxer_t*)>(
        rtsp_muxer_create(+[](void*, int, const void*, int, std::uint32_t, int) { return 0; }, nullptr),
        [](rtsp_muxer_t* value) { rtsp_muxer_destroy(value); });
    if (!muxer)
    {
        tracks_.clear();
        video_transcoder_.reset();
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
            rtsp_muxer_add_media(muxer.get(),
                                 payload_index,
                                 description.rtp_codec,
                                 description.extra.data(),
                                 static_cast<int>(description.extra.size())) < 0)
        {
            tracks_.clear();
            video_transcoder_.reset();
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
            video_transcoder_.reset();
            video_track_id_ = 0;
            stream_.reset();
            return rtsp_server_reply_describe(server, 415, "");
        }
        media_sdp.write(media_text, media_text_size);
        media_sdp << "a=control:trackID=" << description.track.id << "\r\n";
    }

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 1 1 IN IP4 " << connection->local_address() << "\r\n"
        << "s=media_server\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << media_sdp.str();

    spdlog::info("rtsp output describe {}", stream_name_);
    return rtsp_server_reply_describe(server, 200, sdp.str().c_str());
}

int rtsp_output_session::on_setup(rtsp_server_t* server,
                                  std::string_view uri,
                                  std::string_view session,
                                  const rtsp_header_transport_t transports[],
                                  std::size_t count)
{
    if (!stream_)
    {
        const auto prepare_result = prepare_stream(uri);
        if (prepare_result != 0)
        {
            return rtsp_server_reply_setup(server, prepare_result, nullptr, nullptr);
        }
    }
    if (stream_name_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto id = track_id_from_uri(uri);
    if (!id || std::ranges::none_of(tracks_, [id](const rtsp_output_track_description& value) { return value.track.id == *id; }))
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto current_stream = registry_.find(stream_name_);
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

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }
    auto child = std::make_shared<rtsp_output_tcp_session>(
        connection_, registry_, stream_, stream_name_, tracks_, video_transcoder_, video_track_id_);
    return child->startup(server, uri, session, transports, count);
}

int rtsp_output_session::on_play(rtsp_server_t* server, std::string_view uri, std::string_view, const std::int64_t*)
{
    if (stream_name_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
    }
    return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
}

int rtsp_output_session::on_teardown(rtsp_server_t* server, std::string_view)
{
    return rtsp_server_reply_teardown(server, 454);
}

int rtsp_output_session::prepare_stream(std::string_view uri)
{
    tracks_.clear();
    video_transcoder_.reset();
    video_track_id_ = 0;
    stream_.reset();

    stream_name_ = stream_name_from_uri(uri);
    auto stream = registry_.find(stream_name_);
    if (!stream)
    {
        return 404;
    }
    const auto snapshot = stream->tracks();

    std::vector<rtsp_output_track_description> descriptions;
    std::shared_ptr<video_transcoder> prepared_transcoder;
    track_id video_track_id{};
    int next_payload_type = 96;
    for (const auto& track : snapshot)
    {
        if (!supported_track(track))
        {
            continue;
        }

        rtsp_output_track_description description;
        description.track = track;
        if (track.kind == media_kind::video && video_config_.codec == output_video_codec::av1)
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
                return 415;
            }
            description.extra.assign(config.begin(), config.end());
            description.encoding = "AV1";
            description.rtp_codec = RTP_PAYLOAD_AV1;
            description.frequency = 90'000;
            description.payload_type = next_payload_type++;

            auto transcoder = std::make_shared<video_transcoder>();
            if (!transcoder->startup(video_transcoder_config{
                    .input_codec = track.codec,
                    .output_codec = codec_id::av1,
                    .input_codec_config = track.codec_config,
                    .av1 = rtsp_av1_parameters,
                }))
            {
                return 415;
            }
            video_track_id = track.id;
            prepared_transcoder = std::move(transcoder);
        }
        else if (track.codec == codec_id::h264)
        {
            description.extra = h264_annex_b_to_avcc(track.codec_config);
            if (description.extra.empty())
            {
                return 415;
            }
            description.encoding = "H264";
            description.rtp_codec = RTP_PAYLOAD_H264;
            description.frequency = 90'000;
            description.payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::h265)
        {
            description.extra = h265_annex_b_to_hvcc(track.codec_config);
            if (description.extra.empty())
            {
                return 415;
            }
            description.encoding = "H265";
            description.rtp_codec = RTP_PAYLOAD_H265;
            description.frequency = 90'000;
            description.payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::aac)
        {
            description.extra = track.codec_config;
            if (description.extra.empty() || track.clock_rate == 0)
            {
                return 415;
            }
            description.encoding = "MPEG4-GENERIC";
            description.rtp_codec = RTP_PAYLOAD_MP4A;
            description.frequency = static_cast<int>(track.clock_rate);
            description.payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::opus)
        {
            description.encoding = "opus";
            description.rtp_codec = RTP_PAYLOAD_OPUS;
            description.frequency = 48'000;
            description.payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::g711a)
        {
            description.encoding = "PCMA";
            description.rtp_codec = RTP_PAYLOAD_PCMA;
            description.frequency = 8'000;
            description.payload_type = RTP_PAYLOAD_PCMA;
        }
        else if (track.codec == codec_id::g711u)
        {
            description.encoding = "PCMU";
            description.rtp_codec = RTP_PAYLOAD_PCMU;
            description.frequency = 8'000;
            description.payload_type = RTP_PAYLOAD_PCMU;
        }
        else
        {
            continue;
        }

        descriptions.emplace_back(std::move(description));
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
        if (!supported_track(track))
        {
            continue;
        }
        ++supported_count;
        const auto iterator = std::ranges::find_if(tracks_, [id = track.id](const rtsp_output_track_description& value) { return value.track.id == id; });
        if (iterator == tracks_.end() || iterator->track.config_version != track.config_version)
        {
            return false;
        }
    }
    return supported_count == tracks_.size();
}

std::string rtsp_output_session::stream_name_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return {};
    }

    std::string result;
    for (const auto segment : parsed->segments())
    {
        const std::string value(segment);
        if (value.starts_with("trackID="))
        {
            break;
        }
        if (!result.empty())
        {
            result.push_back('/');
        }
        result.append(value);
    }
    return result;
}

std::optional<track_id> rtsp_output_session::track_id_from_uri(std::string_view uri)
{
    const auto parsed = boost::urls::parse_uri_reference(uri);
    if (!parsed)
    {
        return std::nullopt;
    }

    for (const auto segment : parsed->segments())
    {
        const std::string_view value(segment.data(), segment.size());
        constexpr std::string_view prefix = "trackID=";
        if (!value.starts_with(prefix))
        {
            continue;
        }
        const auto text = value.substr(prefix.size());
        track_id id = 0;
        const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error != std::errc{} || pointer != text.data() + text.size())
        {
            return std::nullopt;
        }
        return id;
    }
    return std::nullopt;
}

}    // namespace media_server
