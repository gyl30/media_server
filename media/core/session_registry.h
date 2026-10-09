#ifndef MEDIA_CORE_SESSION_REGISTRY_H
#define MEDIA_CORE_SESSION_REGISTRY_H

#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_server
{

class session
{
   public:
    virtual ~session() = default;

    virtual void shutdown() = 0;
};

struct receiver_identity
{
    std::string stream_name;
    std::string stream_id;
};

class session_registry final
{
   public:
    [[nodiscard]] static session_registry& instance();

    bool add_receiver_session(std::string stream_name, std::string stream_id, std::shared_ptr<session> session);
    [[nodiscard]] std::shared_ptr<session> take_receiver_session(std::string_view stream_name, std::string_view expected_stream_id);
    void remove_receiver_session(std::string_view stream_name, const session& expected);
    [[nodiscard]] std::vector<receiver_identity> receivers() const;
    [[nodiscard]] std::shared_ptr<session> find_receiver_session(std::string_view stream_name, std::string_view expected_stream_id) const;

    bool add_sender_session(std::string stream_name, std::string sender_id, std::string stream_id, std::shared_ptr<session> session);
    [[nodiscard]] std::shared_ptr<session> take_sender_session(std::string_view stream_name,
                                                                      std::string_view sender_id,
                                                                      std::string_view expected_stream_id);
    void remove_sender_session(std::string_view stream_name, std::string_view sender_id, const session& expected);
    void shutdown_all();

   private:
    struct registered_session
    {
        std::string stream_id;
        std::shared_ptr<session> value;
    };

    struct session_entry
    {
        std::optional<registered_session> receiver;
        std::map<std::string, registered_session, std::less<>> sender_sessions;
    };

    session_registry() = default;
    static bool empty(const session_entry& entry);

    mutable std::mutex mutex_;
    std::map<std::string, session_entry, std::less<>> sessions_;
    bool stopping_{};
};

}    // namespace media_server

#endif
