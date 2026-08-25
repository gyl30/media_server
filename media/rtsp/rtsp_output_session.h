#ifndef MEDIA_RTSP_RTSP_OUTPUT_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_SESSION_H

#include <memory>
#include <string>
#include <vector>

#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_track.h"
#include "media/codec/output_video_config.h"
#include "media/rtsp/rtsp_server_connection.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_output_session final : public std::enable_shared_from_this<rtsp_output_session>
{
   public:
    rtsp_output_session(std::weak_ptr<rtsp_server_connection> connection, output_video_config video = {});

    void startup();
    void shutdown();

   private:
    friend class rtsp_server;

    [[nodiscard]] std::shared_ptr<const rtsp_server_connection_handler> make_handler();
    [[nodiscard]] std::size_t on_read(std::span<const std::uint8_t> data);
    void safe_shutdown();
    int on_describe(rtsp_server_t* server, std::string_view uri);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    [[nodiscard]] int prepare_stream(std::string_view uri);
    [[nodiscard]] bool description_current() const;

    std::weak_ptr<rtsp_server_connection> connection_;
    output_video_config video_config_;
    std::shared_ptr<media_stream> stream_;
    std::vector<rtsp_output_track_description> tracks_;
    std::shared_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    std::string stream_name_;
    bool closed_{};
};

}    // namespace media_server

#endif
