#ifndef MEDIA_CONTROL_CONTROL_SESSION_REGISTRY_H
#define MEDIA_CONTROL_CONTROL_SESSION_REGISTRY_H

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

namespace media_server
{

class control_session
{
   public:
    virtual ~control_session() = default;

    virtual void shutdown() = 0;
};

class control_session_registry final
{
   public:
    [[nodiscard]] static control_session_registry& instance();

    bool add_receiver_session(std::string stream_name, std::string stream_id, std::shared_ptr<control_session> session);
    template <typename Session>
    [[nodiscard]] std::shared_ptr<Session> take_receiver_session_as(std::string_view stream_name, std::string_view expected_stream_id)
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = sessions_.find(stream_name);
        if (iterator == sessions_.end())
        {
            return {};
        }
        auto session = std::dynamic_pointer_cast<Session>(iterator->second.receiver_session.session);
        if (!session || iterator->second.receiver_session.stream_id != expected_stream_id)
        {
            return {};
        }
        iterator->second.receiver_session = {};
        if (empty(iterator->second))
        {
            sessions_.erase(iterator);
        }
        return session;
    }
    void remove_receiver_session(std::string_view stream_name, const control_session& expected);

    bool add_sender_session(std::string stream_name, std::string sender_id, std::string stream_id, std::shared_ptr<control_session> session);
    [[nodiscard]] std::shared_ptr<control_session> take_sender_session(std::string_view stream_name,
                                                                      std::string_view sender_id,
                                                                      std::string_view expected_stream_id);
    void remove_sender_session(std::string_view stream_name, std::string_view sender_id, const control_session& expected);
    void shutdown_all();

   private:
    struct registered_session
    {
        std::string stream_id;
        std::shared_ptr<control_session> session;
    };

    struct session_entry
    {
        registered_session receiver_session;
        std::map<std::string, registered_session, std::less<>> sender_sessions;
    };

    control_session_registry() = default;
    static bool empty(const session_entry& entry);

    mutable std::mutex mutex_;
    std::map<std::string, session_entry, std::less<>> sessions_;
    bool stopping_{};
};

}    // namespace media_server

#endif
