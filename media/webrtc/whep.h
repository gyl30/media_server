#ifndef MEDIA_WEBRTC_WHEP_H
#define MEDIA_WEBRTC_WHEP_H

#include <map>
#include <string>
#include <string_view>
#include <memory>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "config.h"

namespace media_server
{
class worker_context;
class media_stream;
}

namespace media_server::whep
{

enum class create_error
{
    none,
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
                                   std::shared_ptr<media_stream> stream,
                                   std::string_view token,
                                   std::string_view offer_sdp,
                                   const config& application_config,
                                   const boost::asio::ip::tcp::socket& socket,
                                   boost::asio::yield_context yield);
[[nodiscard]] bool contains(std::string_view session_id);
[[nodiscard]] bool remove(std::string_view session_id);
void shutdown();

}    // namespace media_server::whep

#endif
