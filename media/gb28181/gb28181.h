#ifndef MEDIA_GB28181_GB28181_H
#define MEDIA_GB28181_GB28181_H

#include <string>
#include <string_view>
#include <cstdint>
#include <optional>

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

[[nodiscard]] gb28181_create_error create(boost::asio::io_context& owner,
                                          std::string stream_name,
                                          gb28181_description description,
                                          std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint,
                                          std::optional<std::uint16_t> remote_rtcp_port);
[[nodiscard]] bool remove(std::string_view stream_name);
[[nodiscard]] gb28181_output_create_error create_output(boost::asio::io_context& owner,
                                                         std::string stream_name,
                                                         std::string output_id,
                                                         bool rtcp,
                                                         gb28181_description description);
[[nodiscard]] bool remove_output(std::string_view stream_name, std::string_view output_id);

}    // namespace media_server::gb28181

#endif
