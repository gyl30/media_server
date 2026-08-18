#include "media/rtsp/rtsp_output_tcp_session.h"

#include "media/codec/codec_utils.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include "rtp-packet.h"
#include "rtp-payload.h"
#include "rtsp-header-transport.h"
#include "rtsp-muxer.h"
#include "rtsp-server.h"
}

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <boost/url/parse.hpp>
#include <charconv>
#include <cstring>
#include <optional>
#include <random>
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

std::uint32_t random_u32()
{
    std::random_device device;
    return (static_cast<std::uint32_t>(device()) << 16U) ^ static_cast<std::uint32_t>(device());
}

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

rtsp_output_tcp_session::rtsp_output_tcp_session(std::weak_ptr<rtsp_server_connection> connection,
                                                 stream_registry& registry,
                                                 std::shared_ptr<media_stream> stream,
                                                 std::string stream_name,
                                                 std::vector<rtsp_output_track_description> tracks,
                                                 std::shared_ptr<video_transcoder> video_transcoder,
                                                 track_id video_track_id)
    : connection_(std::move(connection)),
      registry_(registry),
      stream_(std::move(stream)),
      stream_name_(std::move(stream_name)),
      descriptions_(std::move(tracks)),
      video_transcoder_(std::move(video_transcoder)),
      video_track_id_(video_track_id)
{
}

rtsp_output_tcp_session::~rtsp_output_tcp_session()
{
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
    }
}

int rtsp_output_tcp_session::startup(rtsp_server_t* server,
                                     std::string_view uri,
                                     std::string_view session,
                                     const rtsp_header_transport_t transports[],
                                     std::size_t count)
{
    if (closed_ || !create_muxer())
    {
        return rtsp_server_reply_setup(server, 500, nullptr, nullptr);
    }

    const auto result = on_setup(server, uri, session, transports, count);
    if (result != 0 || !std::ranges::any_of(tracks_, [](const auto& item) { return item.second.setup; }))
    {
        return result;
    }

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }

    const auto self = shared_from_this();
    auto handler = std::make_shared<rtsp_server_connection_handler>();
    handler->on_read = [self](std::span<const std::uint8_t> data) { return self->on_control_read(data); };
    handler->on_shutdown = [self]() { self->safe_shutdown(); };
    handler->on_describe = [](rtsp_server_t* handler_server, const char*) { return rtsp_server_reply_describe(handler_server, 455, ""); };
    handler->on_setup = [self](rtsp_server_t* handler_server,
                               const char* handler_uri,
                               const char* handler_session,
                               const rtsp_header_transport_t handler_transports[],
                               std::size_t handler_count)
    { return self->on_setup(handler_server, handler_uri, handler_session, handler_transports, handler_count); };
    handler->on_play = [self](rtsp_server_t* handler_server,
                              const char* handler_uri,
                              const char* handler_session,
                              const std::int64_t* npt,
                              const double*)
    { return self->on_play(handler_server, handler_uri, handler_session, npt); };
    handler->on_teardown = [self](rtsp_server_t* handler_server, const char*, const char* handler_session)
    { return self->on_teardown(handler_server, handler_session); };
    handler->on_get_parameter = [](rtsp_server_t* handler_server, const char*, const char*, const void*, int)
    { return rtsp_server_reply_get_parameter(handler_server, 200, nullptr, 0); };
    connection->set_handler(std::move(handler));
    return result;
}

void rtsp_output_tcp_session::shutdown()
{
    if (const auto connection = connection_.lock())
    {
        connection->shutdown();
    }
}

void rtsp_output_tcp_session::on_tracks(media_track_snapshot_ptr tracks)
{
    if (closed_ || !playing_ || muxer_ == nullptr)
    {
        return;
    }

    if (!apply_tracks(tracks))
    {
        shutdown();
        return;
    }
    reader_handle().async_read(reader_cursor_);
}

void rtsp_output_tcp_session::on_read(media_read_batch batch)
{
    if (closed_ || !playing_ || muxer_ == nullptr)
    {
        return;
    }

    reader_cursor_ = batch.next_cursor;
    if (!apply_tracks(batch.tracks))
    {
        shutdown();
        return;
    }

    for (auto& entry : batch.entries)
    {
        const auto iterator = tracks_.find(entry.frame.track);
        if (iterator == tracks_.end() || !entry.frame.payload || !iterator->second.setup || iterator->second.media_id < 0 ||
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
                shutdown();
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

void rtsp_output_tcp_session::on_end() { shutdown(); }

int rtsp_output_tcp_session::muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t, int)
{
    return static_cast<rtsp_output_tcp_session*>(param)->on_muxer_packet(pid, data, bytes);
}

std::size_t rtsp_output_tcp_session::on_control_read(std::span<const std::uint8_t> data)
{
    const auto connection = connection_.lock();
    return connection ? connection->input(data) : data.size();
}

void rtsp_output_tcp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    reader_.remove();
    reader_ = {};
    reader_cursor_.reset();
    track_revision_ = 0;
    video_transcoder_.reset();
    video_track_id_ = 0;
    stream_.reset();
    descriptions_.clear();
    tracks_.clear();
    if (muxer_ != nullptr)
    {
        rtsp_muxer_destroy(muxer_);
        muxer_ = nullptr;
    }
    spdlog::debug("rtsp output shutdown {}", stream_name_);
}

bool rtsp_output_tcp_session::create_muxer()
{
    if (muxer_ != nullptr)
    {
        return true;
    }

    muxer_ = rtsp_muxer_create(&rtsp_output_tcp_session::muxer_packet_callback, this);
    if (muxer_ == nullptr)
    {
        return false;
    }

    for (const auto& description : descriptions_)
    {
        const auto payload_index = rtsp_muxer_add_payload(muxer_,
                                                          "RTP/AVP",
                                                          description.frequency,
                                                          description.payload_type,
                                                          description.encoding.c_str(),
                                                          0,
                                                          random_u32(),
                                                          0,
                                                          description.extra.data(),
                                                          static_cast<int>(description.extra.size()));
        if (payload_index < 0)
        {
            return false;
        }
        const auto media_id = rtsp_muxer_add_media(
            muxer_, payload_index, description.rtp_codec, description.extra.data(), static_cast<int>(description.extra.size()));
        if (media_id < 0)
        {
            return false;
        }
        tracks_.insert_or_assign(description.track.id,
                                 track_state{
                                     .codec = description.track.codec,
                                     .config_version = description.track.config_version,
                                     .payload_index = payload_index,
                                     .media_id = media_id,
                                 });

    }
    return !tracks_.empty();
}

bool rtsp_output_tcp_session::apply_tracks(const media_track_snapshot_ptr& tracks)
{
    if (!tracks || tracks->revision <= track_revision_)
    {
        return true;
    }

    for (const auto& [id, state] : tracks_)
    {
        if (!state.setup)
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

int rtsp_output_tcp_session::on_setup(rtsp_server_t* server,
                                      std::string_view uri,
                                      std::string_view session,
                                      const rtsp_header_transport_t transports[],
                                      std::size_t count)
{
    if (playing_)
    {
        return rtsp_server_reply_setup(server, 455, nullptr, nullptr);
    }
    if (stream_name_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }

    const auto id = track_id_from_uri(uri);
    if (!id)
    {
        return rtsp_server_reply_setup(server, 404, nullptr, nullptr);
    }
    auto iterator = tracks_.find(*id);
    if (iterator == tracks_.end())
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
        selected->interleaved2 > 255 || !channels_available(*id, selected->interleaved1, selected->interleaved2))
    {
        return rtsp_server_reply_setup(server, 461, nullptr, "RTP/AVP/TCP;unicast;interleaved=0-1");
    }

    if (iterator->second.setup)
    {
        if (session == session_id_ && iterator->second.rtp_channel == selected->interleaved1 && iterator->second.rtcp_channel == selected->interleaved2)
        {
            std::ostringstream transport;
            transport << "RTP/AVP/TCP;unicast;interleaved=" << selected->interleaved1 << '-' << selected->interleaved2;
            return rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.str().c_str());
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

    iterator->second.rtp_channel = static_cast<std::uint8_t>(selected->interleaved1);
    iterator->second.rtcp_channel = static_cast<std::uint8_t>(selected->interleaved2);
    iterator->second.setup = true;

    std::ostringstream transport;
    transport << "RTP/AVP/TCP;unicast;interleaved=" << selected->interleaved1 << '-' << selected->interleaved2;
    return rtsp_server_reply_setup(server, 200, session_id_.c_str(), transport.str().c_str());
}

int rtsp_output_tcp_session::on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt)
{
    if (stream_name_from_uri(uri) != stream_name_)
    {
        return rtsp_server_reply_play(server, 404, nullptr, nullptr, nullptr);
    }
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_play(server, 454, nullptr, nullptr, nullptr);
    }
    const auto current_stream = registry_.find(stream_name_);
    if (!stream_ || !current_stream)
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

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }
    playing_ = true;
    reader_ = stream_->add_reader(shared_from_this(), connection->executor());
    return result;
}

int rtsp_output_tcp_session::on_teardown(rtsp_server_t* server, std::string_view session)
{
    if (session_id_.empty() || session != session_id_)
    {
        return rtsp_server_reply_teardown(server, 454);
    }

    const auto result = rtsp_server_reply_teardown(server, 200);
    shutdown();
    return result;
}

int rtsp_output_tcp_session::on_muxer_packet(int pid, const void* data, int bytes)
{
    if (data == nullptr || bytes <= 0)
    {
        return 0;
    }

    auto iterator = std::find_if(tracks_.begin(), tracks_.end(), [pid](const auto& item) { return item.second.payload_index == pid; });
    if (iterator == tracks_.end() || !iterator->second.setup)
    {
        return 0;
    }

    const auto connection = connection_.lock();
    if (!connection)
    {
        return -1;
    }

    std::array<std::uint8_t, 4> header{};
    header[0] = 0x24;
    header[1] = iterator->second.rtp_channel;
    const auto network_bytes = htons(static_cast<std::uint16_t>(bytes));
    std::memcpy(header.data() + 2, &network_bytes, sizeof(network_bytes));
    connection->write(header);
    connection->write(std::span(static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(bytes)));

    std::array<std::uint8_t, 1500> rtcp{};
    const auto rtcp_bytes = rtsp_muxer_rtcp(muxer_, pid, rtcp.data(), static_cast<int>(rtcp.size()));
    if (rtcp_bytes > 0)
    {
        header[1] = iterator->second.rtcp_channel;
        const auto network_rtcp_bytes = htons(static_cast<std::uint16_t>(rtcp_bytes));
        std::memcpy(header.data() + 2, &network_rtcp_bytes, sizeof(network_rtcp_bytes));
        connection->write(header);
        connection->write(std::span(rtcp.data(), static_cast<std::size_t>(rtcp_bytes)));
    }
    return 0;
}

bool rtsp_output_tcp_session::description_current() const
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
        const auto iterator = tracks_.find(track.id);
        if (iterator == tracks_.end() || iterator->second.config_version != track.config_version)
        {
            return false;
        }
    }
    return supported_count == tracks_.size();
}

bool rtsp_output_tcp_session::channels_available(track_id id, int rtp_channel, int rtcp_channel) const
{
    if (rtp_channel == rtcp_channel)
    {
        return false;
    }
    for (const auto& [track, state] : tracks_)
    {
        if (track == id || !state.setup)
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

std::string rtsp_output_tcp_session::stream_name_from_uri(std::string_view uri)
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

std::optional<track_id> rtsp_output_tcp_session::track_id_from_uri(std::string_view uri)
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
