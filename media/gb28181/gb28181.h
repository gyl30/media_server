#ifndef MEDIA_GB28181_GB28181_H
#define MEDIA_GB28181_GB28181_H

#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/gb28181/gb28181_types.h"

namespace media_server::gb28181
{

[[nodiscard]] int create(boost::asio::io_context& owner, gb28181_input_config config);
[[nodiscard]] int remove(std::string_view stream_name);
[[nodiscard]] int create_output(boost::asio::io_context& owner, gb28181_output_config config);
[[nodiscard]] int remove_output(std::string_view stream_name, std::string_view output_id);

}    // namespace media_server::gb28181

#endif
