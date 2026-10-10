#ifndef MEDIA_WEBRTC_WHIP_H
#define MEDIA_WEBRTC_WHIP_H

#include <string>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "config.h"

namespace media_server
{
class worker_context;
}

namespace media_server::whip
{

enum class create_error
{
    none,
    stream_conflict,
    invalid_offer,
    forbidden,
    internal_error,
};

struct create_result
{
    create_error error{create_error::internal_error};
    std::string session_id;
    std::string answer_sdp;
};

[[nodiscard]] create_result create(worker_context& worker,
                                   std::string_view stream_id,
                                   std::string_view offer_sdp,
                                   const config& application_config,
                                   const boost::asio::ip::tcp::socket& socket,
                                   boost::asio::yield_context yield);
[[nodiscard]] bool remove(std::string_view session_id);
void shutdown();

}    // namespace media_server::whip

#endif
