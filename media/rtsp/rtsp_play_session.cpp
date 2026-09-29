#include <memory>
#include <random>
#include <vector>
#include <cstring>
#include <sstream>
#include <utility>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/rtsp/rtsp_uri.h"
#include "media/codec/codec_utils.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_play_session.h"

extern "C"
{
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
std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

[[nodiscard]] bool rtsp_play_track_supported(const media_track& track)
{
    return (track.kind == media_kind::video &&
            (track.codec == codec_id::h264 || track.codec == codec_id::h265)) ||
           (track.kind == media_kind::audio && (track.codec == codec_id::aac ||
                                                (track.codec == codec_id::opus && track.clock_rate == 48'000 &&
                                                 (track.channel_count == 1 || track.channel_count == 2) && track.codec_config.empty()) ||
                                                ((track.codec == codec_id::g711a || track.codec == codec_id::g711u) && track.clock_rate == 8'000 &&
                                                 track.channel_count == 1 && track.codec_config.empty())));
}
}    // namespace

rtsp_play_session::rtsp_play_session(worker_context& worker,
                                     std::string stream_name,
                                     boost::asio::ip::address local_address,
                                     write_handler write)
    : worker_(worker),
      stream_name_(std::move(stream_name)),
      local_address_(std::move(local_address)),
      write_handler_(std::move(write))
{
}

void rtsp_play_session::on_frame(const media_frame& entry)
{
    if (closed_ || !playing_)
    {
        return;
    }

    if (waiting_video_track_)
    {
        if (entry.track != *waiting_video_track_ || !entry.key_frame)
        {
            return;
        }
        waiting_video_track_.reset();
    }

    const auto iterator = track_states_.find(entry.track);
    if (iterator == track_states_.end() || !entry.payload || iterator->second.rtp_channel < 0 || iterator->second.media_id < 0)
    {
        return;
    }

    const auto& state = iterator->second;
    if (state.codec == codec_id::opus || state.codec == codec_id::g711a || state.codec == codec_id::g711u)
    {
        constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
        if ((entry.pts_ns % nanoseconds_per_millisecond) != 0 || (entry.dts_ns % nanoseconds_per_millisecond) != 0)
        {
            spdlog::error("rtsp play audio timestamp precision unsupported track {} codec {} pts_ns {} dts_ns {}",
                          entry.track,
                          to_string(state.codec),
                          entry.pts_ns,
                          entry.dts_ns);
            shutdown_handler_();
            return;
        }

        const auto packet_size = rtp_packet_getsize();
        const auto payload_capacity = packet_size - RTP_FIXED_HEADER;
        if (entry.payload->size() > static_cast<std::size_t>(payload_capacity))
        {
            spdlog::error("rtsp play audio packet too large track {} codec {} bytes {} capacity {}",
                          entry.track,
                          to_string(state.codec),
                          entry.payload->size(),
                          payload_capacity);
            shutdown_handler_();
            return;
        }
    }
    const auto mux_result = rtsp_muxer_input(muxer_,
                                             state.media_id,
                                             ns_to_milliseconds(entry.pts_ns),
                                             ns_to_milliseconds(entry.dts_ns),
                                             entry.payload->data(),
                                             static_cast<int>(entry.payload->size()),
                                             entry.key_frame ? 1 : 0);
    if (mux_result < 0)
    {
        spdlog::error("rtsp play mux failed result {}", mux_result);
        shutdown_handler_();
    }
}

void rtsp_play_session::on_end()
{
    if (!closed_)
    {
        shutdown_handler_();
    }
}

int rtsp_play_session::muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t, int)
{
    return static_cast<rtsp_play_session*>(param)->on_muxer_packet(pid, data, bytes);
}

bool rtsp_play_session::on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data)
{
    if (session_id_.empty())
    {
        return false;
    }
    if (muxer_ == nullptr || data.empty())
    {
        return true;
    }

    for (const auto& [id, state] : track_states_)
    {
        if (state.rtcp_channel < 0 || state.rtcp_channel != channel)
        {
            continue;
        }
        if (rtsp_muxer_onrtcp(muxer_, state.payload_index, data.data(), static_cast<int>(data.size())) < 0)
        {
            return false;
        }
        return true;
    }
    return true;
}

void rtsp_play_session::shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtsp_play_session::safe_shutdown()
{
    if (stream_)
    {
        stream_->remove_sink(this);
    }
    waiting_video_track_.reset();
    if (stream_)
    {
        spdlog::debug("rtsp play shutdown {}", stream_->name());
        stream_.reset();
    }
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
    write_handler_ = {};
    shutdown_handler_ = {};
}

int rtsp_play_session::on_describe(rtsp_server_t* server, std::string_view uri)
{
    if (!session_id_.empty())
    {
        return rtsp_server_reply_describe(server, 455, "");
    }
    if (rtsp_path_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_describe(server, 404, "");
    }
    const auto prepare_result = prepare_presentation();
    if (prepare_result != 0)
    {
        return rtsp_server_reply_describe(server, prepare_result, "");
    }

    auto control_base = uri.substr(0, uri.find('?'));
    if (control_base.ends_with('/'))
    {
        control_base.remove_suffix(1);
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
        media_sdp << "a=control:" << control_base << "/trackID=" << id << "\r\n";
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

    spdlog::info("rtsp play describe {}", stream_->name());
    return rtsp_server_reply_describe(server, 200, sdp.str().c_str());
}

int rtsp_play_session::on_setup(
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
        if (separator == std::string::npos || path.substr(0, separator) != stream_name_)
        {
            return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
        }
        const auto prepare_result = prepare_presentation();
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

int rtsp_play_session::on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double*)
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
        std::size_t setup_track_count{};
        bool setup_track_path{};
        for (const auto& [id, state] : track_states_)
        {
            if (state.rtp_channel < 0)
            {
                continue;
            }
            ++setup_track_count;
            setup_track_path = setup_track_path || path == stream_->name() + "/trackID=" + std::to_string(id);
        }
        if (setup_track_count != 1 || !setup_track_path)
        {
            return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
        }
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

    const auto result = rtsp_server_reply_play(server, 200, npt, nullptr, nullptr);
    if (result != 0)
    {
        return result;
    }
    playing_ = true;
    waiting_video_track_.reset();
    for (const auto& [id, state] : track_states_)
    {
        if (state.kind == media_kind::video && state.rtp_channel >= 0)
        {
            waiting_video_track_ = id;
            break;
        }
    }
    stream_->add_sink(shared_from_this());
    return 0;
}

int rtsp_play_session::on_teardown(rtsp_server_t* server, std::string_view, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }

    const auto result = rtsp_server_reply_teardown(server, 200);
    return result == 0 ? -1 : result;
}

int rtsp_play_session::on_muxer_packet(int pid, const void* data, int bytes)
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

    write_interleaved(static_cast<std::uint8_t>(iterator->second.rtp_channel), data, static_cast<std::size_t>(bytes));

    std::array<std::uint8_t, 1500> rtcp{};
    const auto rtcp_bytes = rtsp_muxer_rtcp(muxer_, pid, rtcp.data(), static_cast<int>(rtcp.size()));
    if (rtcp_bytes > 0)
    {
        write_interleaved(static_cast<std::uint8_t>(iterator->second.rtcp_channel), rtcp.data(), static_cast<std::size_t>(rtcp_bytes));
    }
    return 0;
}

void rtsp_play_session::write_interleaved(std::uint8_t channel, const void* data, std::size_t bytes)
{
    std::vector<std::uint8_t> packet(4U + bytes);
    packet[0] = 0x24;
    packet[1] = channel;
    const auto network_bytes = htons(static_cast<std::uint16_t>(bytes));
    std::memcpy(packet.data() + 2U, &network_bytes, sizeof(network_bytes));
    std::memcpy(packet.data() + 4U, data, bytes);
    write_handler_(std::move(packet));
}

int rtsp_play_session::presentation_status() const
{
    const auto current_stream = stream_registry::instance().find(stream_->name());
    if (!current_stream)
    {
        return 503;
    }
    if (current_stream.get() != stream_.get())
    {
        return 455;
    }

    return 0;
}

bool rtsp_play_session::channels_available(track_id id, int rtp_channel, int rtcp_channel) const
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

int rtsp_play_session::prepare_presentation()
{
    track_states_.clear();
    waiting_video_track_.reset();
    stream_.reset();
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }

    auto stream = stream_registry::instance().find(stream_name_);
    if (!stream)
    {
        return 404;
    }
    const auto& snapshot = stream->tracks();
    auto* prepared_muxer = rtsp_muxer_create(&rtsp_play_session::muxer_packet_callback, this);
    if (prepared_muxer == nullptr)
    {
        return 500;
    }
    boost::scope::scope_exit cleanup_muxer([&]() { rtsp_muxer_destroy(prepared_muxer); });

    std::map<track_id, track_state> prepared_tracks;
    int next_payload_type = 96;
    for (const auto& track : snapshot)
    {
        if (!rtsp_play_track_supported(track))
        {
            continue;
        }
        std::vector<std::uint8_t> extra;
        const char* encoding{};
        int rtp_codec{-1};
        int frequency{};
        int payload_type{-1};
        if (track.codec == codec_id::h264)
        {
            extra = h264_annex_b_to_avcc(track.codec_config);
            if (extra.empty())
            {
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
        state.kind = track.kind;
        state.codec = track.codec;
        state.payload_index = rtsp_muxer_add_payload(
            prepared_muxer, "RTP/AVP", frequency, payload_type, encoding, 0, random_u32(), 0, extra.data(), static_cast<int>(extra.size()));
        if (state.payload_index < 0)
        {
            return 415;
        }
        state.media_id = rtsp_muxer_add_media(prepared_muxer, state.payload_index, rtp_codec, extra.data(), static_cast<int>(extra.size()));
        if (state.media_id < 0)
        {
            return 415;
        }
        prepared_tracks.emplace(track.id, std::move(state));
    }

    if (prepared_tracks.empty())
    {
        return 415;
    }

    stream_ = std::move(stream);
    track_states_ = std::move(prepared_tracks);
    muxer_ = prepared_muxer;
    cleanup_muxer.set_active(false);
    return 0;
}

}    // namespace media_server
