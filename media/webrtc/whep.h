#ifndef MEDIA_WEBRTC_WHEP_H
#define MEDIA_WEBRTC_WHEP_H

#include <map>
#include <string>
#include <string_view>

#include "config.h"

namespace media_server
{
class worker_context;
}

namespace media_server::whep
{

enum class create_error
{
    none,
    stream_not_found,
    invalid_offer,
    internal_error,
};

struct create_result
{
    create_error error{create_error::internal_error};
    std::string session_id;
    std::string answer_sdp;
};

[[nodiscard]] create_result create(worker_context& worker,
                                   std::string_view stream_name,
                                   std::string_view offer_sdp,
                                   const config& application_config);
[[nodiscard]] bool contains(std::string_view session_id);
[[nodiscard]] bool remove(std::string_view session_id);

}    // namespace media_server::whep

#endif
