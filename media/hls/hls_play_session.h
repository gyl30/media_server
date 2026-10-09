#ifndef MEDIA_HLS_HLS_PLAY_SESSION_H
#define MEDIA_HLS_HLS_PLAY_SESSION_H

#include <memory>
#include <string>
#include <chrono>
#include <string_view>

#include <boost/system/error_code.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/net/worker_context.h"

namespace media_server
{
class hls_segmenter;

class hls_play_session final : public std::enable_shared_from_this<hls_play_session>
{
   public:
    [[nodiscard]] static std::shared_ptr<hls_play_session> create(worker_context& worker,
                                                                  std::string stream_name,
                                                                  std::shared_ptr<hls_segmenter> segmenter);
    [[nodiscard]] static std::shared_ptr<hls_play_session> find(std::string_view secret, std::string_view stream_name);
    [[nodiscard]] const std::string& secret() const noexcept { return secret_; }
    [[nodiscard]] const std::shared_ptr<hls_segmenter>& segmenter() const noexcept { return segmenter_; }

   private:
    hls_play_session(
        worker_context& worker, std::string stream_name, std::string secret, std::shared_ptr<hls_segmenter> segmenter);

    void wait_for_inactivity();
    void handle_inactivity(const boost::system::error_code& error);
    void shutdown();
    void safe_shutdown();

   private:
    static constexpr auto inactivity_timeout = std::chrono::seconds{30};

   private:
    std::string stream_name_;
    std::string secret_;
    std::shared_ptr<hls_segmenter> segmenter_;
    std::chrono::steady_clock::time_point last_activity_;
    worker_context::shutdown_subscription shutdown_subscription_;
    boost::asio::steady_timer timer_;
};

}    // namespace media_server

#endif
