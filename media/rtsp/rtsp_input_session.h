#ifndef MEDIA_RTSP_RTSP_INPUT_SESSION_H
#define MEDIA_RTSP_RTSP_INPUT_SESSION_H

#include <span>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>

#include "media/rtsp/rtsp_input_media.h"
#include "media/rtsp/rtsp_server_session.h"

namespace media_server
{

class rtsp_input_tcp_session;
class rtsp_input_udp_session;

class rtsp_input_session final : public rtsp_server_session
{
   public:
    rtsp_input_session(boost::asio::any_io_executor executor, std::function<void(std::span<const std::uint8_t>)> write);

    void on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data) override;
    int on_setup(rtsp_server_t* server,
                 std::string_view uri,
                 std::string_view session,
                 const rtsp_header_transport_t transports[],
                 std::size_t count) override;
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session) override;
    int on_announce(rtsp_server_t* server, std::string_view uri, const char* sdp, int length) override;
    int on_record(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale) override;
    void shutdown() override;

   private:
    boost::asio::any_io_executor executor_;
    std::function<void(std::span<const std::uint8_t>)> write_handler_;
    std::shared_ptr<rtsp_input_tcp_session> tcp_session_;
    std::shared_ptr<rtsp_input_udp_session> udp_session_;
    std::vector<rtsp_input_track_description> descriptions_;
    std::string stream_name_;
    std::string session_id_;
    bool closed_{};
};

}    // namespace media_server

#endif
