#include "media/webrtc/whep_service.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace media_server
{

whep_service::whep_service(boost::asio::io_context& io, stream_registry& registry, boost::asio::ip::address advertised_address)
    : io_(io), registry_(registry), advertised_address_(std::move(advertised_address)), certificate_(dtls_certificate::create())
{
}

whep_service::~whep_service() { close(); }

bool whep_service::ready() const noexcept { return certificate_ != nullptr; }

whep_create_result whep_service::create(std::string_view stream_name, std::string_view offer_sdp)
{
    spdlog::debug("whep create stream {} offer_bytes {}", stream_name, offer_sdp.size());

    auto stream = registry_.find(stream_name);
    if (!stream)
    {
        spdlog::debug("whep create stream not found {}", stream_name);
        return {.error = whep_create_error::stream_not_found, .session_id = {}, .answer_sdp = {}};
    }
    if (!certificate_)
    {
        spdlog::error("whep create missing dtls certificate");
        return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }

    auto offer = parse_webrtc_offer(offer_sdp);
    if (!offer)
    {
        spdlog::debug("whep create invalid offer stream {}", stream_name);
        return {.error = whep_create_error::invalid_offer, .session_id = {}, .answer_sdp = {}};
    }

    spdlog::debug("whep offer parsed stream {} media_count {} bundle_mid_count {}", stream_name, offer->media.size(), offer->bundle_mids.size());
    for (const auto& media : offer->media)
    {
        spdlog::trace("whep offer media type {} mid {} direction {} protocol {} rtcp_mux {} payload_count {} codec_count {}",
                      media.type,
                      media.mid,
                      media.direction,
                      media.protocol,
                      media.rtcp_mux,
                      media.payload_types.size(),
                      media.codecs.size());
        for (const auto& codec : media.codecs)
        {
            spdlog::trace("whep offer codec mid {} pt {} name {} clock {} channels {} fmtp {}",
                          media.mid,
                          codec.payload_type,
                          codec.encoding_name,
                          codec.clock_rate,
                          codec.channel_count,
                          codec.format_parameters);
        }
    }

    auto session = std::make_shared<whep_session>(io_,
                                                  stream,
                                                  advertised_address_,
                                                  certificate_,
                                                  [this](const whep_session& closed_session)
                                                  {
                                                      const auto iterator = sessions_.find(closed_session.id());
                                                      if (iterator != sessions_.end() && iterator->second.get() == &closed_session)
                                                      {
                                                          spdlog::info("whep session released {}", closed_session.id());
                                                          sessions_.erase(iterator);
                                                      }
                                                  });
    switch (session->start(std::move(*offer)))
    {
        case whep_session_start_error::none:
            break;
        case whep_session_start_error::invalid_offer:
            return {.error = whep_create_error::invalid_offer, .session_id = {}, .answer_sdp = {}};
        case whep_session_start_error::stream_not_ready:
            return {.error = whep_create_error::stream_not_ready, .session_id = {}, .answer_sdp = {}};
        case whep_session_start_error::internal_error:
            return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }

    const auto& session_id = session->id();
    const bool inserted = sessions_.emplace(session_id, session).second;
    if (!inserted)
    {
        spdlog::error("whep session id collision {}", session_id);
        session->close();
        return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }

    spdlog::info("whep session created {} stream {}", session_id, stream_name);
    return {
        .error = whep_create_error::none,
        .session_id = session_id,
        .answer_sdp = session->answer_sdp(),
    };
}

bool whep_service::remove(std::string_view session_id)
{
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end())
    {
        spdlog::debug("whep session remove not found {}", session_id);
        return false;
    }
    auto session = iterator->second;
    sessions_.erase(iterator);
    session->close();
    spdlog::info("whep session removed {}", session_id);
    return true;
}

void whep_service::close()
{
    auto sessions = std::move(sessions_);
    sessions_.clear();
    for (const auto& [id, session] : sessions)
    {
        static_cast<void>(id);
        session->close();
    }
}

}    // namespace media_server
