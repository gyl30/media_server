#ifndef MEDIA_CORE_MEDIA_STREAM_H
#define MEDIA_CORE_MEDIA_STREAM_H

#include "media/core/media_reader.h"
#include "media/core/media_sink.h"

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media_server
{

class media_stream final : public std::enable_shared_from_this<media_stream>
{
   public:
    media_stream(std::string name, boost::asio::any_io_executor owner_executor);

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] std::vector<media_track> tracks() const;

    void add_sink(const std::shared_ptr<media_sink>& sink);
    [[nodiscard]] media_reader_handle add_reader(const std::shared_ptr<media_reader>& reader,
                                                 boost::asio::any_io_executor executor);
    // 仅在新增轨道或实际配置变化时返回 true；只由 stream owner worker 调用。
    bool update_track(media_track track);
    // 只由 stream owner worker 调用。
    void publish(media_frame frame);
    // 只由 stream owner worker 调用。
    void end();

   private:
    struct media_history_entry
    {
        std::uint64_t sequence{};
        std::uint64_t config_version{};
        media_frame frame;
    };

    friend class media_reader_handle;

    void add_sink_on_owner(std::shared_ptr<media_sink> sink);
    void publish_track_snapshot();
    void request_read(const std::shared_ptr<media_reader_state>& state, media_reader_cursor cursor);
    void remove_reader(const std::shared_ptr<media_reader_state>& state);
    void add_reader_on_owner(const std::shared_ptr<media_reader_state>& state);
    void request_read_on_owner(const std::shared_ptr<media_reader_state>& state, media_reader_cursor cursor);
    void remove_reader_on_owner(const std::shared_ptr<media_reader_state>& state);
    void remove_inactive_readers();
    void reset_history();
    void dispatch_reader_tracks(const media_track_snapshot_ptr& tracks);
    void end_readers();
    void append_history(std::uint64_t sequence, const media_frame& frame, const media_track& track);
    void dispatch_pending_readers();
    void complete_reader_from_history(const std::shared_ptr<media_reader_state>& state);
    void deliver_reader_batch(const std::shared_ptr<media_reader_state>& state, media_read_batch batch);
    void dispatch_reader_tracks(const std::shared_ptr<media_reader_state>& state, media_track_snapshot_ptr tracks);
    void dispatch_reader_end(const std::shared_ptr<media_reader_state>& state);
    [[nodiscard]] bool has_video_track() const;

    std::string name_;
    boost::asio::any_io_executor owner_executor_;
    std::map<track_id, media_track> tracks_;
    std::shared_ptr<media_sink> sink_;
    std::vector<std::shared_ptr<media_reader_state>> readers_;
    std::deque<media_history_entry> history_;
    std::optional<std::uint64_t> current_gop_start_sequence_;
    std::size_t current_gop_frames_{};
    std::uint64_t next_history_sequence_{};
    std::uint64_t sink_replay_barrier_sequence_{};
    std::uint64_t track_revision_{};
    std::atomic<media_track_snapshot_ptr> track_snapshot_;
    std::atomic_bool ended_{};
};

}    // namespace media_server

#endif
