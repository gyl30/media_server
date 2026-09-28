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
struct media_history_entry_t
{
    std::uint64_t sequence{};
    std::uint64_t config_version{};
    media_kind kind{};
    Frame frame;
};

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
    // 仅允许已有 track 在固定 codec 内更新；配置变化或编码状态重启时推进版本。只由 stream owner worker 调用。
    bool update_track(media_track track, bool codec_state_reset = false);
    // 只由 stream owner worker 调用。
    void publish(Frame frame);
    // 只由 stream owner worker 调用。
    void end();

   protected:
    using media_history_entry = media_history_entry_t<Frame>;

   private:
    friend class media_reader_t<Frame>;
    friend struct media_worker_history_t<Frame>;

    void publish_track_snapshot();
    void remove_reader(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void add_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void remove_reader_on_owner(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    void remove_worker(const std::shared_ptr<media_worker_history_t<Frame>>& group);
    void reset_history();
    void dispatch_reader_tracks(const media_track_snapshot_ptr& tracks, bool reset_video_gop = false);
    void dispatch_reader_end(const std::shared_ptr<media_reader_state_t<Frame>>& state);
    [[nodiscard]] std::shared_ptr<media_worker_history_t<Frame>> make_worker_history(worker_context& worker);
    void replace_worker_history(const std::shared_ptr<media_worker_history_t<Frame>>& group);

   protected:
    std::string name_;
    worker_context& worker_;
    std::map<track_id, media_track> tracks_;
    std::map<worker_context*, std::shared_ptr<media_worker_history_t<Frame>>> workers_;
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
