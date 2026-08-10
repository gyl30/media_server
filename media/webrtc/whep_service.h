#ifndef MEDIA_WEBRTC_WHEP_SERVICE_H
#define MEDIA_WEBRTC_WHEP_SERVICE_H

#include "media/core/stream_registry.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/whep_session.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace media_server
{

enum class whep_create_error
{
    none,
    stream_not_found,
    stream_not_ready,
    invalid_offer,
    internal_error,
};

struct whep_create_result
{
    whep_create_error error{whep_create_error::internal_error};
    std::string session_id;
    std::string answer_sdp;
};

class whep_service final
{
   public:
    whep_service(boost::asio::io_context& io, stream_registry& registry, boost::asio::ip::address advertised_address);
    ~whep_service();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] whep_create_result create(std::string_view stream_name, std::string_view offer_sdp);
    [[nodiscard]] bool remove(std::string_view session_id);
    void close();

   private:
    boost::asio::io_context& io_;
    stream_registry& registry_;
    boost::asio::ip::address advertised_address_;
    std::shared_ptr<dtls_certificate> certificate_;
    std::map<std::string, std::weak_ptr<whep_session>, std::less<>> sessions_;
};

}    // namespace media_server

#endif
