#ifndef MEDIA_GB28181_GB28181_SESSION_REGISTRY_H
#define MEDIA_GB28181_GB28181_SESSION_REGISTRY_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "media/gb28181/gb28181_session.h"

namespace media_server
{

class gb28181_session_registry final
{
   public:
    static gb28181_session_registry& instance();

    bool add_input(std::string stream_name, std::shared_ptr<gb28181_session> session);
    std::shared_ptr<gb28181_session> take_input(std::string_view stream_name);
    void remove_input(std::string_view stream_name, const gb28181_session& expected);

    bool add_output(std::string stream_name, std::string output_id, std::shared_ptr<gb28181_session> session);
    std::shared_ptr<gb28181_session> take_output(std::string_view stream_name, std::string_view output_id);
    void remove_output(std::string_view stream_name, std::string_view output_id, const gb28181_session& expected);

    void clear();

   private:
    using output_key = std::pair<std::string, std::string>;

    gb28181_session_registry() = default;

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<gb28181_session>, std::less<>> inputs_;
    std::map<output_key, std::shared_ptr<gb28181_session>> outputs_;
};

}    // namespace media_server

#endif
