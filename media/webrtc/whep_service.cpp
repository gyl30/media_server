#include "media/webrtc/whep_service.h"

#include <utility>

namespace media_server
{

whep_service::whep_service(
    boost::asio::io_context& io,
    stream_registry& registry,
    boost::asio::ip::address advertised_address)
    : io_(io),
      registry_(registry),
      advertised_address_(std::move(advertised_address)),
      certificate_(dtls_certificate::create())
{
}

bool whep_service::ready() const noexcept
{
    return certificate_ != nullptr;
}

whep_create_result whep_service::create(std::string_view stream_name, std::string_view offer_sdp)
{
    auto stream = registry_.find(stream_name);
    if (!stream)
    {
        return {.error = whep_create_error::stream_not_found, .session_id = {}, .location = {}, .answer_sdp = {}};
    }
    if (stream->ended() || stream->tracks().empty())
    {
        return {.error = whep_create_error::stream_not_ready, .session_id = {}, .location = {}, .answer_sdp = {}};
    }
    if (!certificate_)
    {
        return {.error = whep_create_error::internal_error, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    auto offer = parse_webrtc_offer(offer_sdp);
    if (!offer)
    {
        return {.error = whep_create_error::invalid_offer, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    auto session = std::make_shared<whep_session>(io_, stream, advertised_address_, certificate_);
    if (!session->start(std::move(*offer)))
    {
        return {.error = whep_create_error::invalid_offer, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    const auto session_id = session->id();
    const auto [iterator, inserted] = sessions_.emplace(session_id, session);
    static_cast<void>(iterator);
    if (!inserted)
    {
        session->close();
        return {.error = whep_create_error::internal_error, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    return {
        .error = whep_create_error::none,
        .session_id = session_id,
        .location = "/whep/session/" + session_id,
        .answer_sdp = session->answer_sdp(),
    };
}

bool whep_service::remove(std::string_view session_id)
{
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end())
    {
        return false;
    }
    iterator->second->close();
    sessions_.erase(iterator);
    return true;
}

void whep_service::close()
{
    for (const auto& [id, session] : sessions_)
    {
        static_cast<void>(id);
        session->close();
    }
    sessions_.clear();
}

}    // namespace media_server
