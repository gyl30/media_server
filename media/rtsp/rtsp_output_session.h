#ifndef MEDIA_RTSP_RTSP_OUTPUT_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_SESSION_H

#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_track.h"
#include "config.h"
#include "media/codec/output_video_config.h"
#include "media/rtsp/rtsp_server_connection.h"

struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class rtsp_output_session final : public std::enable_shared_from_this<rtsp_output_session>
{
   public:
    rtsp_output_session(rtsp_server_connection& connection, boost::asio::any_io_executor executor, const config& config);

    void shutdown();

   private:
    friend class rtsp_server_connection;

    [[nodiscard]] std::shared_ptr<const rtsp_server_connection_handler> make_handler();
    void safe_shutdown();
    int on_describe(rtsp_server_t* server, std::string_view uri);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    [[nodiscard]] int prepare_stream(std::string_view uri);
    [[nodiscard]] bool description_current() const;

    rtsp_server_connection& connection_;
    boost::asio::any_io_executor executor_;
    output_video_config video_config_;
    std::shared_ptr<media_stream> stream_;
    std::vector<rtsp_output_track_description> tracks_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    std::string stream_name_;
    bool closed_{};
};

}    // namespace media_server

#endif
