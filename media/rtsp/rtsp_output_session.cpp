#include <array>
#include <random>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>
#include <optional>
#include <algorithm>
#include <arpa/inet.h>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <spdlog/spdlog.h>

#include "media/rtsp/rtsp_uri.h"
#include "media/codec/codec_utils.h"
#include "media/core/stream_registry.h"
#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_session.h"

extern "C"
{
#include "aom-av1.h"
#include "rtp-packet.h"
#include "rtsp-muxer.h"
#include "rtp-profile.h"
#include "rtp-payload.h"
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

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

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

void rtsp_output_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_ || !playing_ || muxer_ == nullptr)
    {
        return;
    }

    if (!apply_tracks(tracks))
    {
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return;
    }
    reader_handle().async_read(reader_cursor_);
}

void rtsp_output_session::on_read(media_read_batch batch)
{
    if (closed_ || !playing_ || muxer_ == nullptr)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    if (!apply_tracks(batch.tracks))
    {
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto iterator = tracks_.find(entry.frame.track);
        if (iterator == tracks_.end() || !entry.frame.payload || iterator->second.rtp_channel < 0 || iterator->second.media_id < 0 ||
            iterator->second.description.track.config_version != entry.config_version)
        {
            continue;
        }

        const auto& state = iterator->second;
        if (state.description.track.codec == codec_id::opus || state.description.track.codec == codec_id::g711a ||
            state.description.track.codec == codec_id::g711u)
        {
            constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
            if ((entry.frame.pts_ns % nanoseconds_per_millisecond) != 0 || (entry.frame.dts_ns % nanoseconds_per_millisecond) != 0)
            {
                spdlog::error("rtsp audio output timestamp precision unsupported track {} codec {} pts_ns {} dts_ns {}",
                              entry.frame.track,
                              to_string(state.description.track.codec),
                              entry.frame.pts_ns,
                              entry.frame.dts_ns);
                continue;
            }

            const auto packet_size = rtp_packet_getsize();
            const auto payload_capacity = packet_size - RTP_FIXED_HEADER;
            if (entry.frame.payload->size() > static_cast<std::size_t>(payload_capacity))
            {
                spdlog::error("rtsp audio output packet too large track {} codec {} bytes {} capacity {}",
                              entry.frame.track,
                              to_string(state.description.track.codec),
                              entry.frame.payload->size(),
                              payload_capacity);
                continue;
            }
        }
        if (video_transcoder_ && entry.frame.track == video_track_id_)
        {
            std::vector<media_frame> output;
            if (!video_transcoder_->transcode(entry.frame, output))
            {
                spdlog::error("rtsp av1 transcode failed track {}", entry.frame.track);
                error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
                return;
            }
            for (const auto& encoded : output)
            {
                if (!encoded.payload)
                {
                    continue;
                }
                const auto mux_result = rtsp_muxer_input(muxer_,
                                                         state.media_id,
                                                         ns_to_milliseconds(encoded.pts_ns),
                                                         ns_to_milliseconds(encoded.dts_ns),
                                                         encoded.payload->data(),
                                                         static_cast<int>(encoded.payload->size()),
                                                         encoded.key_frame ? 1 : 0);
                if (mux_result < 0)
                {
                    spdlog::error("rtsp output av1 mux failed result {}", mux_result);
                }
            }
            continue;
        }

        const auto mux_result = rtsp_muxer_input(muxer_,
                                                 state.media_id,
                                                 ns_to_milliseconds(entry.frame.pts_ns),
                                                 ns_to_milliseconds(entry.frame.dts_ns),
                                                 entry.frame.payload->data(),
                                                 static_cast<int>(entry.frame.payload->size()),
                                                 entry.frame.key_frame ? 1 : 0);
        if (mux_result < 0)
        {
            spdlog::error("rtsp output mux failed result {}", mux_result);
        }
    }

    if (!closed_)
    {
        reader_handle().async_read(reader_cursor_);
    }
}

void rtsp_output_session::on_end()
{
    if (!closed_)
    {
        error_handle_(boost::asio::error::eof);
    }
}

int rtsp_output_session::muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t, int)
{
    return static_cast<rtsp_output_session*>(param)->on_muxer_packet(pid, data, bytes);
}

void rtsp_output_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (session_id_.empty())
    {
        error_handle_(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
        return;
    }
    if (muxer_ == nullptr || data.empty())
    {
        return;
    }

    for (const auto& [id, state] : tracks_)
    {
        static_cast<void>(id);
        if (state.rtcp_channel < 0 || state.rtcp_channel != channel)
        {
            continue;
        }
        if (rtsp_muxer_onrtcp(muxer_, state.payload_index, data.data(), static_cast<int>(data.size())) < 0)
        {
            error_handle_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        }
        return;
    }
}

void rtsp_output_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

void rtsp_output_session::safe_shutdown()
{
    reader_handle().remove();
    reader_cursor_.reset();
    track_revision_ = 0;
    if (video_transcoder_)
    {
        video_transcoder_->shutdown();
        video_transcoder_.reset();
    }
    video_track_id_ = 0;
    if (stream_)
    {
        spdlog::debug("rtsp output shutdown {}", stream_->name());
        stream_.reset();
    }
    tracks_.clear();
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
    write_ = {};
    error_handle_ = {};
}

int rtsp_output_session::on_describe(rtsp_server_t* server, std::string_view uri)
{
    if (!session_id_.empty())
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
    for (const auto& [id, state] : tracks_)
    {
        static_cast<void>(id);
        const auto& description = state.description;
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
    if (playing_)
    {
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
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

    const auto path = rtsp_path_from_uri(uri);
    auto iterator = std::ranges::find_if(tracks_, [&path, this](const auto& item) {
        return path == stream_->name() + "/" + item.second.description.control;
    });
    if (iterator == tracks_.end())
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto id = iterator->first;
    const auto current_stream = registry::instance().find(stream_->name());
    if (!current_stream)
    {
        return rtsp_server_reply_setup(server, 503, nullptr, nullptr);
    }
    if (current_stream.get() != stream_.get() || !description_current())
    {
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }
    if (!session_id_.empty() && session != session_id_)
    {
        return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
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
    if (selected == nullptr || selected->interleaved1 < 0 || selected->interleaved1 > 255 || selected->interleaved2 < 0 ||
        selected->interleaved2 > 255 || !channels_available(id, selected->interleaved1, selected->interleaved2))
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    if (iterator->second.rtp_channel >= 0)
    {
        if (session == session_id_ && iterator->second.rtp_channel == selected->interleaved1 &&
            iterator->second.rtcp_channel == selected->interleaved2)
        {
            std::ostringstream transport;
            transport << "RTP/AVP/TCP;unicast;interleaved=" << selected->interleaved1 << '-' << selected->interleaved2;
            return rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.str().c_str());
        }
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }

    const auto first_setup = session_id_.empty();
    if (first_setup)
    {
        if (!session.empty())
        {
            return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
        }
        if (!create_muxer())
        {
            return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
        }
        session_id_ = std::to_string(random_u32());
    }

    iterator->second.rtp_channel = selected->interleaved1;
    iterator->second.rtcp_channel = selected->interleaved2;

    std::ostringstream transport;
    transport << "RTP/AVP/TCP;unicast;interleaved=" << selected->interleaved1 << '-' << selected->interleaved2;
    const auto result = rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.str().c_str());
    if (first_setup)
    {
        if (result != 0)
        {
            iterator->second.rtp_channel = -1;
            iterator->second.rtcp_channel = -1;
            session_id_.clear();
            rtsp_muxer_destroy(muxer_);
            muxer_ = nullptr;
        }
        else
        {
            stream_name_.clear();
        }
    }
    return result;
}

int rtsp_output_session::on_play(rtsp_server_t* server,
                                 std::string_view uri,
                                 std::string_view session,
                                 const std::int64_t* npt,
                                 const double*)
{
    if (session_id_.empty())
    {
        if (rtsp_path_from_uri(uri) != stream_name_)
        {
            return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
        }
        return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
    }
    if (rtsp_path_from_uri(uri) != stream_->name())
    {
        return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
    }
    if (session != session_id_)
    {
        return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
    }
    const auto current_stream = registry::instance().find(stream_->name());
    if (!current_stream)
    {
        return rtsp_server_reply_play(server, 503, nullptr, nullptr, nullptr);
    }
    if (current_stream.get() != stream_.get() || !description_current())
    {
        return rtsp_server_reply_play(server, 455, nullptr, nullptr, nullptr);
    }

    if (playing_)
    {
        return rtsp_server_reply_play(server, 200, npt, nullptr, nullptr);
    }

    const auto result = rtsp_server_reply_play(server, 200, npt, nullptr, nullptr);
    if (result != 0)
    {
        return result;
    }

    playing_ = true;
    static_cast<void>(stream_->add_reader(shared_from_this(), executor_));
    return result;
}

int rtsp_output_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }

    const auto result = rtsp_server_reply_teardown(server, 200);
    error_handle_(boost::asio::error::eof);
    return result;
}

int rtsp_output_session::on_muxer_packet(int pid, const void* data, int bytes)
{
    if (data == nullptr || bytes <= 0)
    {
        return 0;
    }

    auto iterator = std::find_if(tracks_.begin(), tracks_.end(), [pid](const auto& item) { return item.second.payload_index == pid; });
    if (iterator == tracks_.end() || iterator->second.rtp_channel < 0)
    {
        return 0;
    }

    std::array<std::uint8_t, 4> header{};
    header[0] = 0x24;
    header[1] = static_cast<std::uint8_t>(iterator->second.rtp_channel);
    const auto network_bytes = htons(static_cast<std::uint16_t>(bytes));
    std::memcpy(header.data() + 2, &network_bytes, sizeof(network_bytes));
    write_(header);
    write_(std::span(static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes)));

    std::array<std::uint8_t, 1500> rtcp{};
    const auto rtcp_bytes = rtsp_muxer_rtcp(muxer_, pid, rtcp.data(), static_cast<int>(rtcp.size()));
    if (rtcp_bytes > 0)
    {
        header[1] = static_cast<std::uint8_t>(iterator->second.rtcp_channel);
        const auto network_rtcp_bytes = htons(static_cast<std::uint16_t>(rtcp_bytes));
        std::memcpy(header.data() + 2, &network_rtcp_bytes, sizeof(network_rtcp_bytes));
        write_(header);
        write_(std::span(rtcp.data(), static_cast<std::size_t>(rtcp_bytes)));
    }
    return 0;
}

bool rtsp_output_session::create_muxer()
{
    if (muxer_ != nullptr)
    {
        return true;
    }

    muxer_ = rtsp_muxer_create(&rtsp_output_session::muxer_packet_callback, this);
    if (muxer_ == nullptr)
    {
        return false;
    }

    for (auto& [id, state] : tracks_)
    {
        static_cast<void>(id);
        const auto& description = state.description;
        state.payload_index = rtsp_muxer_add_payload(muxer_,
                                                     "RTP/AVP",
                                                     description.frequency,
                                                     description.payload_type,
                                                     description.encoding.c_str(),
                                                     0,
                                                     random_u32(),
                                                     0,
                                                     description.extra.data(),
                                                     static_cast<int>(description.extra.size()));
        if (state.payload_index < 0)
        {
            rtsp_muxer_destroy(muxer_);
            muxer_ = nullptr;
            return false;
        }
        state.media_id = rtsp_muxer_add_media(muxer_,
                                              state.payload_index,
                                              description.rtp_codec,
                                              description.extra.data(),
                                              static_cast<int>(description.extra.size()));
        if (state.media_id < 0)
        {
            rtsp_muxer_destroy(muxer_);
            muxer_ = nullptr;
            return false;
        }
    }
    return !tracks_.empty();
}

bool rtsp_output_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return true;
    }

    for (const auto& [id, state] : tracks_)
    {
        if (state.rtp_channel < 0)
        {
            continue;
        }
        const auto current = std::ranges::find_if(tracks->tracks, [id](const media_track& track) { return track.id == id; });
        if (current == tracks->tracks.end() || current->config_version != state.description.track.config_version)
        {
            return false;
        }
    }

    track_revision_ = tracks->revision;
    return true;
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
        const auto iterator = tracks_.find(track.id);
        if (iterator == tracks_.end() || iterator->second.description.track.config_version != track.config_version)
        {
            return false;
        }
    }
    return supported_count == tracks_.size();
}

bool rtsp_output_session::channels_available(track_id id, int rtp_channel, int rtcp_channel) const
{
    if (rtp_channel == rtcp_channel)
    {
        return false;
    }
    for (const auto& [track, state] : tracks_)
    {
        if (track == id || state.rtp_channel < 0)
        {
            continue;
        }
        if (state.rtp_channel == rtp_channel || state.rtp_channel == rtcp_channel || state.rtcp_channel == rtp_channel ||
            state.rtcp_channel == rtcp_channel)
        {
            return false;
        }
    }
    return true;
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

    std::map<track_id, track_state> prepared_tracks;
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
        prepared_tracks.emplace(track.id, track_state{.description = std::move(prepared->description)});
    }

    if (prepared_tracks.empty())
    {
        return 415;
    }

    stream_ = std::move(stream);
    tracks_ = std::move(prepared_tracks);
    video_transcoder_ = std::move(prepared_transcoder);
    video_track_id_ = video_track_id;
    return 0;
}

}    // namespace media_server
