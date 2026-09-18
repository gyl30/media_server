#ifndef MEDIA_HLS_HLS_PLAY_SESSION_H
#define MEDIA_HLS_HLS_PLAY_SESSION_H

#include <mutex>
#include <memory>
#include <string>
#include <chrono>
#include <string_view>

#include <boost/system/error_code.hpp>
#include <boost/asio/steady_timer.hpp>

namespace media_server
{
class worker_context;

class hls_play_session final : public std::enable_shared_from_this<hls_play_session>
{
   public:
    [[nodiscard]] static std::shared_ptr<hls_play_session> create(worker_context& worker, std::string stream_id, std::string stream_name);
    [[nodiscard]] static std::shared_ptr<hls_play_session> find(std::string_view secret, std::string_view stream_name);
    static void shutdown_all();

    [[nodiscard]] const std::string& stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] const std::string& stream_name() const noexcept { return stream_name_; }
    [[nodiscard]] const std::string& secret() const noexcept { return secret_; }

    [[nodiscard]] bool refresh();
    [[nodiscard]] bool mark_streaming();

   private:
    hls_play_session(worker_context& worker, std::string stream_id, std::string stream_name, std::string secret);

    void wait_for_inactivity();
    void handle_inactivity(const boost::system::error_code& error);
    [[nodiscard]] bool matches(std::string_view stream_name) const;

   private:
    static constexpr auto inactivity_timeout = std::chrono::seconds{30};
    mutable std::mutex mutex_;
    std::string stream_id_;
    std::string stream_name_;
    std::string secret_;
    std::chrono::steady_clock::time_point last_activity_;
    bool streaming_{};
    bool expired_{};
    boost::asio::steady_timer timer_;
};

}    // namespace media_server

#endif
