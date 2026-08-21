#ifndef MEDIA_GB28181_GB28181_SERVICE_H
#define MEDIA_GB28181_GB28181_SERVICE_H

#include "media/core/stream_registry.h"

#include <boost/asio/any_io_executor.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace media_server
{

class gb28181_udp_session;

enum class gb28181_create_error
{
    none,
    duplicate_stream,
    stream_conflict,
    invalid_sdp,
    internal_error,
};

class gb28181_service final
{
   public:
    explicit gb28181_service(stream_registry& registry);
    ~gb28181_service();

    [[nodiscard]] gb28181_create_error create(boost::asio::any_io_executor executor, std::string stream_name, std::string_view sdp);
    [[nodiscard]] bool remove(std::string_view stream_name);
    void shutdown();

   private:
    stream_registry& registry_;
    std::mutex sessions_mutex_;
    std::map<std::string, std::weak_ptr<gb28181_udp_session>, std::less<>> sessions_;
    bool closed_{};
};

}    // namespace media_server

#endif
