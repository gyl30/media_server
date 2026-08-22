#ifndef MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H

#include "media/core/media_stream.h"
#include "media/gb28181/gb28181_sdp.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/udp.hpp>

#include <memory>
#include <string>
#include <vector>

namespace media_server
{

class gb28181_output_media;
class udp_socket;

class gb28181_udp_output_session final : public std::enable_shared_from_this<gb28181_udp_output_session>
{
   public:
    gb28181_udp_output_session(boost::asio::any_io_executor executor,
                               std::shared_ptr<media_stream> stream,
                               gb28181_description description);

    [[nodiscard]] bool startup();
    void shutdown();

   private:
    void send_packet(std::vector<std::uint8_t> packet);
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    gb28181_description description_;
    boost::asio::ip::udp::endpoint remote_endpoint_;
    std::shared_ptr<udp_socket> socket_;
    std::shared_ptr<gb28181_output_media> media_;
    bool closed_{};
};

}    // namespace media_server

#endif
