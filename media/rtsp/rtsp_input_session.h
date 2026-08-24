#ifndef MEDIA_RTSP_RTSP_INPUT_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <string_view>

#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_input_media.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "rtsp-media.h"
}

namespace media_server
{

class rtsp_input_session final : public std::enable_shared_from_this<rtsp_input_session>
{
   public:
    rtsp_input_session(std::weak_ptr<rtsp_server_connection> connection, stream_registry& registry);

    void shutdown();

   private:
    friend class rtsp_server;

    [[nodiscard]] std::size_t on_read(std::span<const std::uint8_t> data);
    int on_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_record(rtsp_server_t* server, std::string_view session);
    int on_teardown(rtsp_server_t* server, std::string_view session);
    void safe_shutdown();

    static std::optional<media_track> track_from_format(const rtsp_media_t& media, const rtsp_media_t::avformat_t& format);

    std::weak_ptr<rtsp_server_connection> connection_;
    stream_registry& registry_;
    std::vector<rtsp_input_track_description> descriptions_;
    std::string stream_name_;
    std::string session_id_;
    bool announced_{};
    bool closed_{};
};

}    // namespace media_server

#endif
