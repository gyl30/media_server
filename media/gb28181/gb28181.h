#ifndef MEDIA_GB28181_GB28181_H
#define MEDIA_GB28181_GB28181_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/gb28181/gb28181_types.h"

namespace media_server::gb28181
{

enum class gb28181_create_error
{
    none,
    duplicate_stream,
    stream_conflict,
    invalid_configuration,
    internal_error,
};

enum class gb28181_output_create_error
{
    none,
    duplicate_output,
    stream_not_found,
    unsupported_stream,
    invalid_configuration,
    internal_error,
};

[[nodiscard]] gb28181_create_error create(boost::asio::io_context& owner, gb28181_input_config config);
[[nodiscard]] bool remove(std::string_view stream_name);
[[nodiscard]] gb28181_output_create_error create_output(boost::asio::io_context& owner, gb28181_output_config config);
[[nodiscard]] bool remove_output(std::string_view stream_name, std::string_view output_id);

}    // namespace media_server::gb28181

#endif
