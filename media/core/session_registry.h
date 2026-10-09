#ifndef MEDIA_CORE_SESSION_REGISTRY_H
#define MEDIA_CORE_SESSION_REGISTRY_H

#include <map>
#include <functional>
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
    // 按身份开始关闭接收会话：槽位保留并标记关闭，会话清理完成后移除槽位时调用 closed。
    // 槽位不存在或属于其他代时返回空且不调用 closed；并发关闭共享同一次完成。
    [[nodiscard]] std::shared_ptr<session> begin_close_receiver(std::string_view stream_name,
                                                                std::string_view expected_stream_id,
                                                                std::function<void()> closed);
    // 会话在资源清理完成后调用，移除自身槽位并通知等待关闭的请求。
    void remove_receiver_session(std::string_view stream_name, const session& expected);
    [[nodiscard]] bool receiver_open(std::string_view stream_name, const session& expected) const;
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
        bool closing{};
        std::vector<std::function<void()>> closed;
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
