#ifndef MEDIA_RTSP_RTSP_PLAY_SESSION_H
#define MEDIA_RTSP_RTSP_PLAY_SESSION_H

#include <map>
#include <span>
#include <memory>
#include <string>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
#include <functional>
#include <string_view>

#include <boost/asio/ip/address.hpp>

#include "media/core/media_sink.h"

struct rtsp_muxer_t;
struct rtsp_server_t;
struct rtsp_header_transport_t;

namespace media_server
{

class worker_context;
class media_stream;
class rtsp_play_session final : public media_sink, public std::enable_shared_from_this<rtsp_play_session>
{
   public:
    using write_handler = std::function<void(std::vector<std::uint8_t>)>;
    rtsp_play_session(worker_context& worker,
                      std::string stream_name,
                      boost::asio::ip::address local_address,
                      write_handler write);

   public:
    void set_shutdown_handler(std::function<void()> handler) { shutdown_handler_ = std::move(handler); }

   public:
    void shutdown();

   public:
    [[nodiscard]] worker_context& worker() noexcept override { return worker_; }
    void on_frame(const media_frame& frame) override;
    void on_end() override;

   public:
    [[nodiscard]] bool on_interleaved(std::uint8_t channel, std::span<const std::uint8_t> data);
    int on_describe(rtsp_server_t* server, std::string_view uri);
    int on_setup(
        rtsp_server_t* server, std::string_view uri, std::string_view session, const rtsp_header_transport_t transports[], std::size_t count);
    int on_play(rtsp_server_t* server, std::string_view uri, std::string_view session, const std::int64_t* npt, const double* scale);
    int on_teardown(rtsp_server_t* server, std::string_view uri, std::string_view session);

   private:
    struct track_state
    {
        media_kind kind{};
        codec_id codec{};
        int clock_rate{};
        int payload_index{-1};
        int media_index{-1};
        int rtp_channel{-1};
        int rtcp_channel{-1};
    };

    struct rtcp_sync
    {
        std::uint64_t ntp;
        std::int64_t pts;
    };

   private:
    static int muxer_packet_callback(void* param, int payload_index, const void* data, int bytes, std::uint32_t timestamp, int flags);

   private:
    [[nodiscard]] int prepare_presentation();
    int on_muxer_packet(int payload_index, const void* data, int bytes);
    void write_interleaved(std::uint8_t channel, const void* data, std::size_t bytes);
    [[nodiscard]] bool stream_current() const;
    [[nodiscard]] bool channels_available(track_id id, int rtp_channel, int rtcp_channel) const;

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::string stream_name_;
    boost::asio::ip::address local_address_;
    write_handler write_handler_;
    std::function<void()> shutdown_handler_;
    std::shared_ptr<media_stream> stream_;
    std::map<track_id, track_state> track_states_;
    std::optional<track_id> waiting_video_track_;
    std::optional<rtcp_sync> rtcp_sync_;
    rtsp_muxer_t* muxer_{};
    std::string session_id_;
    bool playing_{};
};

}    // namespace media_server

#endif
