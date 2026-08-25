#ifndef MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H
#define MEDIA_GB28181_GB28181_UDP_OUTPUT_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <optional>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "media/core/media_stream.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_session.h"

namespace media_server
{

class gb28181_output_media;
class udp_socket;

class gb28181_udp_output_session final : public gb28181_session,
                                         public std::enable_shared_from_this<gb28181_udp_output_session>
{
   public:
    gb28181_udp_output_session(boost::asio::any_io_executor executor,
                               std::shared_ptr<media_stream> stream,
                               gb28181_description description,
                               std::string output_id,
                               bool rtcp_enabled);

    [[nodiscard]] bool startup();
    void shutdown() override;

   private:
    struct udp_socket_pair
    {
        std::shared_ptr<udp_socket> rtp;
        std::shared_ptr<udp_socket> rtcp;
    };

    [[nodiscard]] std::optional<udp_socket_pair> prepare_udp_sockets(boost::asio::ip::address bind_address);
    void send_packet(std::vector<std::uint8_t> packet);
    void wait_rtcp();
    void safe_shutdown();

    boost::asio::any_io_executor executor_;
    std::shared_ptr<media_stream> stream_;
    std::string stream_name_;
    std::string output_id_;
    gb28181_description description_;
    boost::asio::ip::udp::endpoint remote_rtp_endpoint_;
    boost::asio::ip::udp::endpoint remote_rtcp_endpoint_;
    boost::asio::steady_timer rtcp_timer_;
    std::shared_ptr<udp_socket> rtp_socket_;
    std::shared_ptr<udp_socket> rtcp_socket_;
    std::shared_ptr<gb28181_output_media> media_;
    void* rtcp_sender_{};
    bool rtcp_enabled_{};
    bool rtcp_started_{};
    bool closed_{};
};

}    // namespace media_server

#endif
