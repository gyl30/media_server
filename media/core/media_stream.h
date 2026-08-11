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
    explicit media_stream(std::string name, boost::asio::any_io_executor owner_executor = {});

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] std::vector<media_track> tracks() const;

    void add_sink(const std::shared_ptr<media_sink>& sink, boost::asio::any_io_executor executor = {});
    void remove_sink(const media_sink& sink);
    // track_ids 为空表示动态消费全部轨道；非空集合在 reader 生命周期内保持固定。
    [[nodiscard]] media_reader_handle add_reader(const std::shared_ptr<media_reader>& reader,
                                                 boost::asio::any_io_executor executor,
                                                 std::vector<track_id> track_ids = {});
    // 仅在新增轨道或实际配置变化时返回 true；只由 stream owner worker 调用。
    bool update_track(media_track track);
    // 只由 stream owner worker 调用。
    void publish(media_frame frame);
    // 只由 stream owner worker 调用。
    void end();

   private:
    struct sink_state
    {
        std::weak_ptr<media_sink> sink;
        boost::asio::any_io_executor executor;
        std::atomic_bool active{true};
    };

    struct sink_group
    {
        boost::asio::any_io_executor executor;
        std::shared_ptr<const std::vector<std::shared_ptr<sink_state>>> sinks;
    };

    struct media_history_entry
    {
        std::uint64_t sequence{};
        std::uint64_t config_version{};
        media_frame frame;
    };

    friend class media_reader_handle;

    void add_sink_on_owner(std::shared_ptr<media_sink> sink, boost::asio::any_io_executor executor);
    void remove_sink_on_owner(const media_sink* sink);
    void remove_inactive_sinks();
    void publish_sink_snapshot();
    void publish_track_snapshot();
    [[nodiscard]] bool has_sink(const media_sink& sink) const;
    [[nodiscard]] bool local_executor(const boost::asio::any_io_executor& executor) const;
    void replay_sink(const std::shared_ptr<sink_state>& state,
                     std::vector<media_track> tracks,
                     std::vector<media_frame> frames);
    void dispatch_track(const media_track& track);
    void dispatch_frame(const media_frame& frame);
    void request_read(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation);
    void remove_reader(const std::shared_ptr<media_reader_state>& state);
    void add_reader_on_owner(const std::shared_ptr<media_reader_state>& state);
    void request_read_on_owner(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation);
    void remove_reader_on_owner(const std::shared_ptr<media_reader_state>& state);
    void remove_inactive_readers();
    void reset_reader_history();
    void reset_readers(track_id changed_track, media_kind kind);
    void end_readers();
    void append_reader_history(std::uint64_t sequence, const media_frame& frame, const media_track& track);
    void dispatch_pending_readers(std::uint64_t sequence, const media_frame& frame);
    void complete_reader_from_history(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation);
    void deliver_reader_frame(const std::shared_ptr<media_reader_state>& state,
                              media_reader_generation generation,
                              std::uint64_t sequence,
                              media_frame frame);
    void dispatch_reader_reset(const std::shared_ptr<media_reader_state>& state,
                               std::vector<media_track> tracks,
                               media_reader_generation generation);
    void dispatch_reader_end(const std::shared_ptr<media_reader_state>& state, media_reader_generation generation);
    [[nodiscard]] bool has_video_track() const;

    std::string name_;
    boost::asio::any_io_executor owner_executor_;
    std::map<track_id, media_track> tracks_;
    std::vector<sink_group> sink_groups_;
    std::vector<media_frame> gop_cache_;
    std::vector<std::shared_ptr<media_reader_state>> readers_;
    std::deque<media_history_entry> reader_history_;
    std::optional<std::uint64_t> current_gop_start_sequence_;
    std::size_t current_gop_frames_{};
    std::uint64_t next_reader_sequence_{};
    std::atomic<std::shared_ptr<const std::vector<media_track>>> track_snapshot_;
    std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<sink_state>>>> sink_snapshot_;
    std::atomic_bool ended_{};
};

}    // namespace media_server

#endif
