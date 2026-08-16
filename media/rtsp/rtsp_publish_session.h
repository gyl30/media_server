#ifndef MEDIA_RTSP_RTSP_PUBLISH_SESSION_H
#define MEDIA_RTSP_RTSP_PUBLISH_SESSION_H

#include "media/core/stream_registry.h"
#include "media/net/tcp_connection.h"
#include "media/net/udp_socket.h"

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
#include "avpkt2bs.h"
#include "rtp-over-rtsp.h"
#include "rtsp-media.h"
#include "rtsp-server.h"
}

struct rtsp_demuxer_t;
struct avpacket_t;

namespace media_server
{

class rtsp_publish_session final : public std::enable_shared_from_this<rtsp_publish_session>
{
   public:
    rtsp_publish_session(std::shared_ptr<tcp_connection> connection, stream_registry& registry, std::vector<std::uint8_t> initial_data);
    ~rtsp_publish_session();

    void startup();
    void shutdown();

   private:
    struct media_state
    {
        std::size_t media_index{};
        rtsp_demuxer_t* demuxer{};
        std::optional<media_track> track;
        bool setup{};
        int rtp_channel{-1};
        int rtcp_channel{-1};
        std::shared_ptr<udp_socket> rtp_socket;
        std::shared_ptr<udp_socket> rtcp_socket;
        boost::asio::ip::udp::endpoint client_endpoint;
    };

    static int send_callback(void* param, const void* data, std::size_t bytes);
    static int announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length);
    static int setup_callback(void* param,
                              rtsp_server_t* server,
                              const char* uri,
                              const char* session,
                              const rtsp_header_transport_t transports[],
                              std::size_t count);
    static int record_callback(void* param,
                               rtsp_server_t* server,
                               const char* uri,
                               const char* session,
                               const std::int64_t* npt,
                               const double* scale);
    static int options_callback(void* param, rtsp_server_t* server, const char* uri);
    static int teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session);
    static int get_parameter_callback(void* param,
                                      rtsp_server_t* server,
                                      const char* uri,
                                      const char* session,
                                      const void* content,
                                      int bytes);
    static void rtp_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes);
    static int packet_callback(void* param, avpacket_t* packet);

    void on_tcp_read(std::span<const std::uint8_t> data);
    void on_rtp(std::uint8_t channel, const void* data, std::uint16_t bytes);
    void on_udp_rtp(std::size_t media_index, std::span<const std::uint8_t> data, const boost::asio::ip::udp::endpoint& endpoint);
    int on_announce(std::string_view uri, const char* sdp, int length);
    int on_setup(std::string_view uri,
                 std::string_view session,
                 const rtsp_header_transport_t transports[],
                 std::size_t count);
    int on_record(std::string_view uri, std::string_view session);
    int on_packet(avpacket_t* packet);
    bool update_track_from_packet(const avpacket_t& packet);
    static std::optional<media_track> track_from_format(const rtsp_media_t& media, const rtsp_media_t::avformat_t& format);
    static std::string stream_name_from_uri(std::string_view uri);
    void reset_media();
    void safe_shutdown();

    std::shared_ptr<tcp_connection> connection_;
    stream_registry& registry_;
    std::vector<std::uint8_t> initial_data_;
    rtsp_server_t* server_{};
    std::vector<rtsp_media_t> media_descriptions_;
    std::array<media_state, 2> media_{};
    std::size_t media_count_{};
    std::shared_ptr<media_stream> stream_;
    avpkt2bs_t bitstream_{};
    rtp_over_rtsp_t interleaved_{};
    std::string stream_name_;
    std::string session_id_;
    bool announced_{};
    bool recording_{};
    bool closed_{};
};

}    // namespace media_server

#endif
