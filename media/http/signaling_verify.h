#ifndef MEDIA_HTTP_SIGNALING_VERIFY_H
#define MEDIA_HTTP_SIGNALING_VERIFY_H

#include <string_view>

#include <boost/asio/spawn.hpp>

#include "config.h"

namespace media_server
{

[[nodiscard]] bool valid_stream_token(std::string_view token) noexcept;
[[nodiscard]] bool valid_stream_id(std::string_view stream_id) noexcept;
[[nodiscard]] bool verify_stream(const config& application_config,
                                 std::string_view token,
                                 std::string_view operation,
                                 std::string_view stream_id,
                                 boost::asio::yield_context yield);

}    // namespace media_server

#endif
