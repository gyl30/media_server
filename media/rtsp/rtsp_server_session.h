#ifndef MEDIA_RTSP_RTSP_SERVER_SESSION_H
#define MEDIA_RTSP_RTSP_SERVER_SESSION_H

#include <span>
#include <functional>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <boost/system/error_code.hpp>

extern "C"
{
#include "rtsp-server.h"
}

namespace media_server
{

class rtsp_server_session
{
   public:
    virtual ~rtsp_server_session() = default;

    void set_error_handler(std::function<void(boost::system::error_code)> handler) { error_handler_ = std::move(handler); }

    virtual void on_interleaved(std::uint8_t, std::span<const std::uint8_t>) = 0;
    virtual int on_describe(rtsp_server_t* server, std::string_view) { return rtsp_server_reply_describe(server, 501, ""); }
    virtual int on_setup(rtsp_server_t* server,
                         std::string_view,
                         std::string_view,
                         const rtsp_header_transport_t[],
                         std::size_t)
    {
        return rtsp_server_reply_setup(server, 501, nullptr, nullptr);
    }
    virtual int on_play(rtsp_server_t* server, std::string_view, std::string_view, const std::int64_t*, const double*)
    {
        return rtsp_server_reply_play(server, 501, nullptr, nullptr, nullptr);
    }
    virtual int on_teardown(rtsp_server_t* server, std::string_view, std::string_view) { return rtsp_server_reply_teardown(server, 501); }
    virtual int on_announce(rtsp_server_t* server, std::string_view, const char*, int) { return rtsp_server_reply_announce(server, 501); }
    virtual int on_record(rtsp_server_t* server, std::string_view, std::string_view, const std::int64_t*, const double*)
    {
        return rtsp_server_reply_record(server, 501, nullptr, nullptr);
    }
    virtual void shutdown() = 0;

   protected:
    std::function<void(boost::system::error_code)> error_handler_;
};

}    // namespace media_server

#endif
