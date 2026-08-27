#ifndef MEDIA_RTSP_RTSP_OUTPUT_SESSION_H
#define MEDIA_RTSP_RTSP_OUTPUT_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#include <boost/asio/any_io_executor.hpp>

#include "media/codec/video_transcoder.h"
#include "media/rtsp/rtsp_output_track.h"
#include "media/rtsp/rtsp_server_session.h"
#include "config.h"
#include "media/codec/output_video_config.h"

namespace media_server
{

class rtsp_output_tcp_session;

class rtsp_output_session final : public rtsp_server_session
{
   public:
    rtsp_output_session(boost::asio::any_io_executor executor,
                        const config& config,
                        std::string local_address,
                        std::function<void(std::span<const std::uint8_t>)> write,
                        std::function<void()> request_shutdown);

    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data) override;
    int on_describe(rtsp_server_t* server, std::string_view uri) override;
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count) override;
    int on_play(rtsp_server_t* server,
                std::string_view uri,
                std::string_view session,
                const std::int64_t* npt,
                const double* scale) override;
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session) override;
    int on_get_parameter(rtsp_server_t* server, std::string_view uri, std::string_view session, const void* content, int bytes) override;
    void shutdown() override;

   private:
    [[nodiscard]] int prepare_stream(std::string_view uri);
    [[nodiscard]] bool description_current() const;

    boost::asio::any_io_executor executor_;
    output_video_config video_config_;
    std::string local_address_;
    std::function<void(std::span<const std::uint8_t>)> write_;
    std::function<void()> request_shutdown_;
    std::shared_ptr<rtsp_output_tcp_session> tcp_session_;
    std::shared_ptr<media_stream> stream_;
    std::vector<rtsp_output_track_description> tracks_;
    std::unique_ptr<video_transcoder> video_transcoder_;
    track_id video_track_id_{};
    std::string stream_name_;
    bool closed_{};
};

}    // namespace media_server

#endif
