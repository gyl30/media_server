#ifndef MEDIA_CORE_STREAM_REGISTRY_H
#define MEDIA_CORE_STREAM_REGISTRY_H

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

#include "media/core/media_stream.h"
#include "media/core/runtime_event.h"

namespace media_server
{

class stream_session
{
   public:
    virtual ~stream_session() = default;

    virtual void shutdown(runtime_end_reason reason = runtime_end_reason::requested, std::string error = {}) = 0;
    [[nodiscard]] virtual std::string_view stream_id() const noexcept { return {}; }
};

class stream_registry final
{
   public:
    [[nodiscard]] static stream_registry& instance();

    bool add(const std::shared_ptr<media_stream>& stream);
    void remove(const media_stream& expected);
    [[nodiscard]] std::shared_ptr<media_stream> find(std::string_view name) const;

    bool add_receiver_session(std::string stream_name, std::shared_ptr<stream_session> session);
    [[nodiscard]] std::shared_ptr<stream_session> take_receiver_session(std::string_view stream_name);
    template <typename Session>
    [[nodiscard]] std::shared_ptr<Session> take_receiver_session_as(std::string_view stream_name,
                                                                    std::string_view expected_stream_id = {})
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = streams_.find(stream_name);
        if (iterator == streams_.end())
        {
            return {};
        }
        auto session = std::dynamic_pointer_cast<Session>(iterator->second.receiver_session);
        if (!session || (!expected_stream_id.empty() && session->stream_id() != expected_stream_id))
        {
            return {};
        }
        iterator->second.receiver_session.reset();
        if (empty(iterator->second))
        {
            streams_.erase(iterator);
        }
        return session;
    }
    void remove_receiver_session(std::string_view stream_name, const stream_session& expected);

    bool add_sender_session(std::string stream_name, std::string sender_id, std::shared_ptr<stream_session> session);
    [[nodiscard]] std::shared_ptr<stream_session> take_sender_session(std::string_view stream_name,
                                                                      std::string_view sender_id,
                                                                      std::string_view expected_stream_id = {});
    void remove_sender_session(std::string_view stream_name, std::string_view sender_id, const stream_session& expected);

    void shutdown_sessions(runtime_end_reason reason = runtime_end_reason::server_shutdown);
    void clear();

   private:
    struct stream_entry
    {
        std::shared_ptr<media_stream> stream;
        std::shared_ptr<stream_session> receiver_session;
        std::map<std::string, std::shared_ptr<stream_session>, std::less<>> sender_sessions;
    };

    stream_registry() = default;

    static bool empty(const stream_entry& entry);

    mutable std::mutex mutex_;
    std::map<std::string, stream_entry, std::less<>> streams_;
};

}    // namespace media_server

#endif
