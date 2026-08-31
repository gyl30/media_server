#include <array>
#include <memory>
#include <random>
#include <vector>
#include <cstring>
#include <sstream>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/error.hpp>

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
#include "rtp-payload.h"
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

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

[[nodiscard]] bool rtsp_output_track_supported(const media_track& track)
{
    return (track.kind == media_kind::video && (track.codec == codec_id::h264 || track.codec == codec_id::h265)) ||
           (track.kind == media_kind::audio && (track.codec == codec_id::aac ||
                                                (track.codec == codec_id::opus && track.clock_rate == 48'000 &&
                                                 (track.channel_count == 1 || track.channel_count == 2) && track.codec_config.empty()) ||
                                                ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) && track.clock_rate == 8'000 &&
                                                 track.channel_count == 1 && track.codec_config.empty())));
}
}    // namespace

rtsp_output_session::rtsp_output_session(boost::asio::any_io_executor executor,
                                         output_video_codec video_codec,
                                         boost::asio::ip::address local_address,
                                         std::function<void(std::span<const std::uint8_t>)> write)
    : executor_(std::move(executor)), video_codec_(video_codec), local_address_(std::move(local_address)), write_handler_(std::move(write))
{
}

void rtsp_output_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_)
    {
        return;
    }

    if (!apply_tracks(tracks))
    {
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return;
    }
    reader_handle().async_read(reader_cursor_);
}

void rtsp_output_session::on_read(media_read_batch batch)
{
    if (closed_)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    if (!apply_tracks(batch.tracks))
    {
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto iterator = track_states_.find(entry.frame.track);
        if (iterator == track_states_.end() || !entry.frame.payload || iterator->second.rtp_channel < 0 || iterator->second.media_id < 0 ||
            iterator->second.config_version != entry.config_version)
        {
            continue;
        }

        const auto& state = iterator->second;
        if (state.codec == codec_id::opus || state.codec == codec_id::g711a || state.codec == codec_id::g711u)
        {
            constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
            if ((entry.frame.pts_ns % nanoseconds_per_millisecond) != 0 || (entry.frame.dts_ns % nanoseconds_per_millisecond) != 0)
            {
                spdlog::error("rtsp audio output timestamp precision unsupported track {} codec {} pts_ns {} dts_ns {}",
                              entry.frame.track,
                              to_string(state.codec),
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
                              to_string(state.codec),
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
                error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
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

    reader_handle().async_read(reader_cursor_);
}

void rtsp_output_session::on_end()
{
    if (!closed_)
    {
        error_handler_(boost::asio::error::eof);
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
        error_handler_(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
        return;
    }
    if (muxer_ == nullptr || data.empty())
    {
        return;
    }

    for (const auto& [id, state] : track_states_)
    {
        static_cast<void>(id);
        if (state.rtcp_channel < 0 || state.rtcp_channel != channel)
        {
            continue;
        }
        if (rtsp_muxer_onrtcp(muxer_, state.payload_index, data.data(), static_cast<int>(data.size())) < 0)
        {
            error_handler_(boost::system::errc::make_error_code(boost::system::errc::io_error));
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
    if (video_transcoder_)
    {
        video_transcoder_->shutdown();
    }
    if (stream_)
    {
        spdlog::debug("rtsp output shutdown {}", stream_->name());
    }
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
    write_handler_ = {};
    error_handler_ = {};
}

int rtsp_output_session::on_describe(rtsp_server_t* server, std::string_view uri)
{
    if (!session_id_.empty())
    {
        return rtsp_server_reply_describe(server, 455, "");
    }
    const auto prepare_result = prepare_presentation(uri);
    if (prepare_result != 0)
    {
        return rtsp_server_reply_describe(server, prepare_result, "");
    }

    std::ostringstream media_sdp;
    for (const auto& [id, state] : track_states_)
    {
        std::uint16_t sequence{};
        std::uint32_t timestamp{};
        const char* media_text{};
        int media_text_size{};
        if (rtsp_muxer_getinfo(muxer_, state.payload_index, &sequence, &timestamp, &media_text, &media_text_size) != 0)
        {
            return rtsp_server_reply_describe(server, 415, "");
        }
        media_sdp.write(media_text, media_text_size);
        media_sdp << "a=control:trackID=" << id << "\r\n";
    }

    std::ostringstream sdp;
    const auto address_type = local_address_.is_v4() ? "IP4" : "IP6";
    sdp << "v=0\r\n"
        << "o=- 1 1 IN " << address_type << ' ' << local_address_.to_string() << "\r\n"
        << "s=media_server\r\n"
        << "c=IN " << address_type << ' ' << local_address_.to_string() << "\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << media_sdp.str();

    spdlog::info("rtsp output describe {}", stream_->name());
    return rtsp_server_reply_describe(server, 200, sdp.str().c_str());
}

int rtsp_output_session::on_setup(
    rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count)
{
    if (playing_)
    {
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }
    const auto path = rtsp_path_from_uri(uri);
    if (!stream_)
    {
        const auto separator = path.rfind('/');
        if (separator == std::string::npos)
        {
            return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
        }
        const auto prepare_result = prepare_presentation(path.substr(0, separator));
        if (prepare_result != 0)
        {
            return rtsp_server_reply_setup(server, prepare_result, nullptr, nullptr);
        }
    }

    auto iterator = std::ranges::find_if(
        track_states_, [&path, this](const auto& item) { return path == stream_->name() + "/trackID=" + std::to_string(item.first); });
    if (iterator == track_states_.end())
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    const auto id = iterator->first;
    if (const auto status = presentation_status(); status != 0)
    {
        return rtsp_server_reply_setup(server, status, nullptr, nullptr);
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

    const auto transport = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(selected->interleaved1) + '-' + std::to_string(selected->interleaved2);
    if (iterator->second.rtp_channel >= 0)
    {
        if (session == session_id_ && iterator->second.rtp_channel == selected->interleaved1 &&
            iterator->second.rtcp_channel == selected->interleaved2)
        {
            return rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.c_str());
        }
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }

    if (session_id_.empty())
    {
        if (!session.empty())
        {
            return rtsp_server_reply_setup(server, 454, nullptr, nullptr);
        }
        session_id_ = std::to_string(random_u32());
    }

    iterator->second.rtp_channel = selected->interleaved1;
    iterator->second.rtcp_channel = selected->interleaved2;
    return rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.c_str());
}

int rtsp_output_session::on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double*)
{
    const auto path = rtsp_path_from_uri(uri);
    if (session_id_.empty())
    {
        if (stream_ && path != stream_->name())
        {
            return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
        }
        return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
    }
    if (path != stream_->name())
    {
        return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
    }
    if (session != session_id_)
    {
        return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
    }
    if (const auto status = presentation_status(); status != 0)
    {
        return rtsp_server_reply_play(server, status, nullptr, nullptr, nullptr);
    }

    if (playing_)
    {
        return rtsp_server_reply_play(server, 200, npt, nullptr, nullptr);
    }

    static_cast<void>(rtsp_server_reply_play(server, 200, npt, nullptr, nullptr));
    playing_ = true;
    static_cast<void>(stream_->add_reader(shared_from_this(), executor_));
    return 0;
}

int rtsp_output_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }

    const auto result = rtsp_server_reply_teardown(server, 200);
    error_handler_(boost::asio::error::eof);
    return result;
}

int rtsp_output_session::on_muxer_packet(int pid, const void* data, int bytes)
{
    if (data == nullptr || bytes <= 0)
    {
        return 0;
    }

    auto iterator = std::find_if(track_states_.begin(), track_states_.end(), [pid](const auto& item) { return item.second.payload_index == pid; });
    if (iterator == track_states_.end() || iterator->second.rtp_channel < 0)
    {
        return 0;
    }

    std::array<std::uint8_t, 4> header{};
    header[0] = 0x24;
    header[1] = static_cast<std::uint8_t>(iterator->second.rtp_channel);
    const auto network_bytes = htons(static_cast<std::uint16_t>(bytes));
    std::memcpy(header.data() + 2, &network_bytes, sizeof(network_bytes));
    write_handler_(header);
    write_handler_(std::span(static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes)));

    std::array<std::uint8_t, 1500> rtcp{};
    const auto rtcp_bytes = rtsp_muxer_rtcp(muxer_, pid, rtcp.data(), static_cast<int>(rtcp.size()));
    if (rtcp_bytes > 0)
    {
        header[1] = static_cast<std::uint8_t>(iterator->second.rtcp_channel);
        const auto network_rtcp_bytes = htons(static_cast<std::uint16_t>(rtcp_bytes));
        std::memcpy(header.data() + 2, &network_rtcp_bytes, sizeof(network_rtcp_bytes));
        write_handler_(header);
        write_handler_(std::span(rtcp.data(), static_cast<std::size_t>(rtcp_bytes)));
    }
    return 0;
}

bool rtsp_output_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return true;
    }

    for (const auto& [id, state] : track_states_)
    {
        if (state.rtp_channel < 0)
        {
            continue;
        }
        const auto current = std::ranges::find_if(tracks->tracks, [id](const media_track& track) { return track.id == id; });
        if (current == tracks->tracks.end() || current->config_version != state.config_version)
        {
            return false;
        }
    }

    track_revision_ = tracks->revision;
    return true;
}

int rtsp_output_session::presentation_status() const
{
    const auto current_stream = registry::instance().find(stream_->name());
    if (!current_stream)
    {
        return 503;
    }
    if (current_stream.get() != stream_.get())
    {
        return 455;
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
        const auto iterator = track_states_.find(track.id);
        if (iterator == track_states_.end() || iterator->second.config_version != track.config_version)
        {
            return 455;
        }
    }
    return supported_count == track_states_.size() ? 0 : 455;
}

bool rtsp_output_session::channels_available(track_id id, int rtp_channel, int rtcp_channel) const
{
    if (rtp_channel == rtcp_channel)
    {
        return false;
    }
    for (const auto& [track, state] : track_states_)
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

int rtsp_output_session::prepare_presentation(std::string_view uri)
{
    track_states_.clear();
    if (video_transcoder_)
    {
        video_transcoder_->shutdown();
        video_transcoder_.reset();
    }
    video_track_id_ = 0;
    stream_.reset();
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }

    auto stream = registry::instance().find(rtsp_path_from_uri(uri));
    if (!stream)
    {
        return 404;
    }
    const auto snapshot = stream->tracks();

    auto prepared_muxer = std::unique_ptr<rtsp_muxer_t, void (*)(rtsp_muxer_t*)>(rtsp_muxer_create(&rtsp_output_session::muxer_packet_callback, this),
                                                                                 [](rtsp_muxer_t* value) { rtsp_muxer_destroy(value); });
    if (!prepared_muxer)
    {
        return 500;
    }

    std::map<track_id, track_state> prepared_tracks;
    std::unique_ptr<video_transcoder> prepared_transcoder;
    const auto shutdown_prepared_transcoder = [&prepared_transcoder]()
    {
        if (prepared_transcoder)
        {
            prepared_transcoder->shutdown();
        }
    };
    track_id video_track_id{};
    int next_payload_type = 96;
    for (const auto& track : snapshot)
    {
        if (!rtsp_output_track_supported(track))
        {
            continue;
        }

        std::vector<std::uint8_t> extra;
        const char* encoding{};
        int rtp_codec{-1};
        int frequency{};
        int payload_type{-1};
        if (track.kind == media_kind::video && video_codec_ == output_video_codec::av1)
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
                shutdown_prepared_transcoder();
                return 415;
            }
            extra.assign(config.begin(), config.end());
            encoding = "AV1";
            rtp_codec = RTP_PAYLOAD_AV1;
            frequency = 90'000;
            payload_type = next_payload_type++;

            auto transcoder = std::make_unique<video_transcoder>();
            if (!transcoder->startup(video_transcoder_config{
                    .input_codec = track.codec,
                    .output_codec = codec_id::av1,
                    .input_codec_config = track.codec_config,
                    .av1 = rtsp_av1_parameters,
                }))
            {
                shutdown_prepared_transcoder();
                return 415;
            }
            shutdown_prepared_transcoder();
            video_track_id = track.id;
            prepared_transcoder = std::move(transcoder);
        }
        else if (track.codec == codec_id::h264)
        {
            extra = h264_annex_b_to_avcc(track.codec_config);
            if (extra.empty())
            {
                shutdown_prepared_transcoder();
                return 415;
            }
            encoding = "H264";
            rtp_codec = RTP_PAYLOAD_H264;
            frequency = 90'000;
            payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::h265)
        {
            extra = h265_annex_b_to_hvcc(track.codec_config);
            if (extra.empty())
            {
                shutdown_prepared_transcoder();
                return 415;
            }
            encoding = "H265";
            rtp_codec = RTP_PAYLOAD_H265;
            frequency = 90'000;
            payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::aac)
        {
            extra = track.codec_config;
            if (extra.empty() || track.clock_rate == 0)
            {
                shutdown_prepared_transcoder();
                return 415;
            }
            encoding = "MPEG4-GENERIC";
            rtp_codec = RTP_PAYLOAD_MP4A;
            frequency = static_cast<int>(track.clock_rate);
            payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::opus)
        {
            encoding = "opus";
            rtp_codec = RTP_PAYLOAD_OPUS;
            frequency = 48'000;
            payload_type = next_payload_type++;
        }
        else if (track.codec == codec_id::g711a)
        {
            encoding = "PCMA";
            rtp_codec = RTP_PAYLOAD_PCMA;
            frequency = 8'000;
            payload_type = RTP_PAYLOAD_PCMA;
        }
        else if (track.codec == codec_id::g711u)
        {
            encoding = "PCMU";
            rtp_codec = RTP_PAYLOAD_PCMU;
            frequency = 8'000;
            payload_type = RTP_PAYLOAD_PCMU;
        }

        track_state state;
        state.codec = track.codec;
        state.config_version = track.config_version;
        state.payload_index = rtsp_muxer_add_payload(
            prepared_muxer.get(), "RTP/AVP", frequency, payload_type, encoding, 0, random_u32(), 0, extra.data(), static_cast<int>(extra.size()));
        if (state.payload_index < 0)
        {
            shutdown_prepared_transcoder();
            return 415;
        }
        state.media_id = rtsp_muxer_add_media(prepared_muxer.get(), state.payload_index, rtp_codec, extra.data(), static_cast<int>(extra.size()));
        if (state.media_id < 0)
        {
            shutdown_prepared_transcoder();
            return 415;
        }
        prepared_tracks.emplace(track.id, std::move(state));
    }

    if (prepared_tracks.empty())
    {
        shutdown_prepared_transcoder();
        return 415;
    }

    stream_ = std::move(stream);
    track_states_ = std::move(prepared_tracks);
    video_transcoder_ = std::move(prepared_transcoder);
    video_track_id_ = video_track_id;
    muxer_ = prepared_muxer.release();
    return 0;
}

}    // namespace media_server
