#ifndef MEDIA_GB28181_GB28181_SERVICE_H
#define MEDIA_GB28181_GB28181_SERVICE_H

#include "media/core/stream_registry.h"

#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace media_server
{

class io_context_pool;

enum class gb28181_create_error
{
    none,
    duplicate_stream,
    stream_conflict,
    invalid_sdp,
    internal_error,
};

enum class gb28181_output_create_error
{
    none,
    duplicate_output,
    stream_not_found,
    unsupported_stream,
    invalid_sdp,
    internal_error,
};

class gb28181_service final
{
   public:
    explicit gb28181_service(stream_registry& registry, io_context_pool* workers = nullptr);
    ~gb28181_service();

    [[nodiscard]] gb28181_create_error create(boost::asio::any_io_executor executor, std::string stream_name, std::string_view sdp);
    [[nodiscard]] bool remove(std::string_view stream_name);
    [[nodiscard]] gb28181_output_create_error create_output(boost::asio::any_io_executor executor,
                                                            std::string stream_name,
                                                            std::string output_id,
                                                            std::string_view sdp);
    [[nodiscard]] bool remove_output(std::string_view stream_name, std::string_view output_id);
    void shutdown();

   private:
    struct state;

    stream_registry& registry_;
    io_context_pool* workers_{};
    std::shared_ptr<state> state_;
};

}    // namespace media_server

#endif
