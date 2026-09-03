#ifndef MEDIA_RTMP_RTMP_INPUT_SESSION_H
#define MEDIA_RTMP_RTMP_INPUT_SESSION_H

#include <span>
#include <chrono>
#include <memory>
#include <string>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>

#include <boost/asio/steady_timer.hpp>

#include "media/rtmp/rtmp_timestamp.h"
#include "media/core/media_stream.h"

struct flv_demuxer_t;

namespace media_server
{

class worker_context;

class rtmp_input_session final : public std::enable_shared_from_this<rtmp_input_session>
{
   public:
    using shutdown_handler = std::function<void()>;

    rtmp_input_session(worker_context& worker,
                       std::string stream_name,
                       std::chrono::milliseconds initial_tracks_timeout,
                       shutdown_handler on_shutdown);
    ~rtmp_input_session();

    bool startup();
    void shutdown();

    int on_video(const void* data, std::size_t bytes, std::uint32_t timestamp);
    int on_audio(const void* data, std::size_t bytes, std::uint32_t timestamp);
    int on_script(std::span<const std::uint8_t> data);

   private:
    static int demux_callback(void* param, int codec, const void* data, std::size_t bytes, std::uint32_t pts, std::uint32_t dts, int flags);

    int on_flv_demux(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags);
    int handle_video_config(int codec, std::span<const std::uint8_t> data);
    int handle_audio_config(int codec, std::span<const std::uint8_t> data);
    int initialize_g711_track(int codec);
    int publish_media(int codec, std::span<const std::uint8_t> data, std::uint32_t pts, std::uint32_t dts, int flags);
    void try_initialize_tracks();

    worker_context& worker_;
    boost::asio::steady_timer initial_tracks_timer_;
    std::chrono::milliseconds initial_tracks_timeout_;
    std::string stream_name_;
    std::shared_ptr<media_stream> stream_;
    shutdown_handler shutdown_handler_;
    flv_demuxer_t* demuxer_{};
    rtmp_timestamp_state timestamp_;
    std::optional<media_track> initial_video_track_;
    std::optional<media_track> initial_audio_track_;
    bool expected_audio_{};
    bool metadata_received_{};
    bool tracks_initialized_{};
    bool closed_{};
};

}    // namespace media_server

#endif
