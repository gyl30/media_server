#ifndef MEDIA_GB28181_GB28181_OUTPUT_MEDIA_H
#define MEDIA_GB28181_GB28181_OUTPUT_MEDIA_H

#include <map>
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>

#include <boost/asio/any_io_executor.hpp>

#include "media/core/media_reader.h"
#include "media/core/media_stream.h"

struct rtsp_muxer_t;

namespace media_server
{

class gb28181_output_media final : public media_reader, public std::enable_shared_from_this<gb28181_output_media>
{
   public:
    using packet_handler = std::function<void(std::vector<std::uint8_t>)>;
    using end_handler = std::function<void()>;

    gb28181_output_media(boost::asio::any_io_executor executor,
                         std::shared_ptr<media_stream> stream,
                         std::uint8_t payload_type,
                         std::uint32_t ssrc,
                         packet_handler on_packet,
                         end_handler on_end);
    ~gb28181_output_media() override;

    [[nodiscard]] static bool supported_tracks(const std::vector<media_track>& tracks);

    [[nodiscard]] bool startup();
    void shutdown();

    void on_tracks(media_track_snapshot_ptr tracks) override;
    void on_read(media_read_batch batch) override;
    void on_end() override;

   private:
    struct track_state
    {
        media_kind kind{};
        codec_id codec{};
        std::uint64_t config_version{};
        int media_id{-1};
    };

    static int muxer_packet_callback(void* param, int pid, const void* data, int bytes, std::uint32_t timestamp, int flags);

    void safe_shutdown();
    [[nodiscard]] bool create_muxer(const std::vector<media_track>& tracks);
    [[nodiscard]] bool apply_tracks(const media_track_snapshot_ptr& tracks);
    int on_muxer_packet(const void* data, int bytes);

    boost::asio::any_io_executor executor_;
    std::shared_ptr<media_stream> stream_;
    std::uint8_t payload_type_{};
    std::uint32_t ssrc_{};
    packet_handler packet_handler_;
    end_handler end_handler_;
    rtsp_muxer_t* muxer_{};
    media_reader_handle reader_;
    std::map<track_id, track_state> track_states_;
    media_reader_cursor reader_cursor_;
    std::uint64_t track_revision_{};
    bool waiting_for_key_frame_{true};
    bool closed_{};
};

}    // namespace media_server

#endif
