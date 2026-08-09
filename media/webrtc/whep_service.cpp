#include "media/webrtc/whep_service.h"

#include <spdlog/spdlog.h>

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
    spdlog::debug("whep create stream {} offer_bytes {}", stream_name, offer_sdp.size());

    auto stream = registry_.find(stream_name);
    if (!stream)
    {
        spdlog::debug("whep create stream not found {}", stream_name);
        return {.error = whep_create_error::stream_not_found, .session_id = {}, .location = {}, .answer_sdp = {}};
    }
    if (stream->ended() || stream->tracks().empty())
    {
        spdlog::debug("whep create stream not ready {}", stream_name);
        return {.error = whep_create_error::stream_not_ready, .session_id = {}, .location = {}, .answer_sdp = {}};
    }
    if (!certificate_)
    {
        spdlog::error("whep create missing dtls certificate");
        return {.error = whep_create_error::internal_error, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    auto offer = parse_webrtc_offer(offer_sdp);
    if (!offer)
    {
        spdlog::debug("whep create invalid offer stream {}", stream_name);
        return {.error = whep_create_error::invalid_offer, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    spdlog::debug("whep offer parsed stream {} media_count {} bundle_mid_count {}", stream_name, offer->media.size(), offer->bundle_mids.size());
    for (const auto& media : offer->media)
    {
        spdlog::trace(
            "whep offer media type {} mid {} direction {} protocol {} rtcp_mux {} payload_count {} codec_count {}",
            media.type,
            media.mid,
            media.direction,
            media.protocol,
            media.rtcp_mux,
            media.payload_types.size(),
            media.codecs.size());
        for (const auto& codec : media.codecs)
        {
            spdlog::trace(
                "whep offer codec mid {} pt {} name {} clock {} channels {} fmtp {}",
                media.mid,
                codec.payload_type,
                codec.encoding_name,
                codec.clock_rate,
                codec.channel_count,
                codec.format_parameters);
        }
    }

    auto session = std::make_shared<whep_session>(io_, stream, advertised_address_, certificate_);
    if (!session->start(std::move(*offer)))
    {
        spdlog::debug("whep session start failed stream {}", stream_name);
        return {.error = whep_create_error::invalid_offer, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    const auto session_id = session->id();
    const auto [iterator, inserted] = sessions_.emplace(session_id, session);
    static_cast<void>(iterator);
    if (!inserted)
    {
        spdlog::error("whep session id collision {}", session_id);
        session->close();
        return {.error = whep_create_error::internal_error, .session_id = {}, .location = {}, .answer_sdp = {}};
    }

    spdlog::info("whep session created {} stream {}", session_id, stream_name);
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
        spdlog::debug("whep session remove not found {}", session_id);
        return false;
    }
    iterator->second->close();
    sessions_.erase(iterator);
    spdlog::info("whep session removed {}", session_id);
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
