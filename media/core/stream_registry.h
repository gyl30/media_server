#ifndef MEDIA_CORE_STREAM_REGISTRY_H
#define MEDIA_CORE_STREAM_REGISTRY_H

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

#include "media/core/singleton.h"
#include "media/core/media_stream.h"

namespace media_server
{

class stream_session
{
   public:
    virtual ~stream_session() = default;

    virtual void shutdown() = 0;
};

class stream_registry final
{
    friend class singleton<stream_registry>;

   public:
    bool add(const std::shared_ptr<media_stream>& stream);
    void remove(const media_stream& expected);
    [[nodiscard]] std::shared_ptr<media_stream> find(std::string_view name) const;

    bool add_input_session(std::string stream_name, std::shared_ptr<stream_session> session);
    [[nodiscard]] std::shared_ptr<stream_session> take_input_session(std::string_view stream_name);
    void remove_input_session(std::string_view stream_name, const stream_session& expected);

    bool add_output_session(std::string stream_name, std::string output_id, std::shared_ptr<stream_session> session);
    [[nodiscard]] std::shared_ptr<stream_session> take_output_session(std::string_view stream_name, std::string_view output_id);
    void remove_output_session(std::string_view stream_name, std::string_view output_id, const stream_session& expected);

    void clear();

   private:
    struct stream_entry
    {
        std::shared_ptr<media_stream> stream;
        std::shared_ptr<stream_session> input_session;
        std::map<std::string, std::shared_ptr<stream_session>, std::less<>> output_sessions;
    };

    stream_registry() = default;

    static bool empty(const stream_entry& entry);

    mutable std::mutex mutex_;
    std::map<std::string, stream_entry, std::less<>> streams_;
};

using registry = singleton<stream_registry>;

}    // namespace media_server

#endif
