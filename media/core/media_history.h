#ifndef MEDIA_CORE_MEDIA_HISTORY_H
#define MEDIA_CORE_MEDIA_HISTORY_H

#include <map>
#include <deque>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "media/net/worker_context.h"
#include "media/core/media_reader.h"

namespace media_server
{

class worker_context;

template <typename Frame>
class media_history : public std::enable_shared_from_this<media_history<Frame>>
{
   public:
    media_history(std::string name, worker_context& worker);

   public:
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] std::vector<media_track> tracks() const;

    void add_reader(const std::shared_ptr<media_reader_t<Frame>>& reader, worker_context& worker);
    // 仅用于发布完整初始轨道集合；成功后 track id/kind/codec 固定。只由 stream owner worker 调用。
    bool set_tracks(std::vector<media_track> tracks);
    // 仅允许已有 track 在固定 codec 内更新配置；实际配置变化时返回 true。只由 stream owner worker 调用。
    bool update_track(media_track track);
    // 只由 stream owner worker 调用。
    void publish(Frame frame);
    // 只由 stream owner worker 调用。
    void end();

   protected:
    struct media_history_entry
    {
        std::uint64_t sequence{};
        std::uint64_t config_version{};
        Frame frame;
    };

   private:
    friend class media_reader_t<Frame>;

    void publish_track_snapshot();
    void request_read(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_reader_cursor cursor);
    void remove_reader(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void add_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void request_read_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_reader_cursor cursor);
    void remove_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void remove_inactive_readers();
    void reset_history();
    void dispatch_reader_tracks(const media_track_snapshot_ptr& tracks);
    void end_readers();
    void append_history(std::uint64_t sequence, const Frame& frame, const media_track& track);
    void dispatch_pending_readers();
    void complete_reader_from_history(const std::shared_ptr<media_reader_state_t<Frame>>& state, bool waited_for_media);
    void deliver_reader_batch(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_read_batch_t<Frame> batch);
    void dispatch_reader_tracks(const std::shared_ptr<media_reader_state_t<Frame>>& state, media_track_snapshot_ptr tracks);
    void dispatch_reader_end(const std::shared_ptr<media_reader_state_t<Frame>>& state);

   protected:
    std::string name_;
    worker_context& worker_;
    std::map<track_id, media_track> tracks_;
    std::vector<std::shared_ptr<media_reader_state_t<Frame>>> readers_;
    std::deque<media_history_entry> history_;
    std::optional<std::uint64_t> current_gop_start_sequence_;
    std::size_t current_gop_frames_{};
    std::uint64_t next_history_sequence_{};
    std::uint64_t track_revision_{};
    std::atomic<media_track_snapshot_ptr> track_snapshot_;
    // 仅用于 owner worker 内阻止 end 后到达的 reader/sink 请求，不参与 registry 可发现性。
    bool ended_{};
};

}    // namespace media_server

#include "media/core/media_history_impl.h"

#endif
