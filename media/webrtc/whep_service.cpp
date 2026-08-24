#include <utility>

#include <spdlog/spdlog.h>

#include "media/webrtc/whep_service.h"

namespace media_server
{

whep_service::whep_service(stream_registry& registry, boost::asio::ip::address advertised_address, output_video_config video)
    : registry_(registry), advertised_address_(std::move(advertised_address)), video_config_(video), certificate_(dtls_certificate::create())
{
}

whep_service::~whep_service() { shutdown(); }

bool whep_service::ready() const noexcept { return certificate_ != nullptr; }

whep_create_result whep_service::create(boost::asio::any_io_executor executor, std::string_view stream_name, std::string_view offer_sdp)
{
    spdlog::debug("whep create stream {} offer_bytes {}", stream_name, offer_sdp.size());

    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
        }
    }

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

    auto session =
        std::make_shared<whep_session>(std::move(executor), stream, advertised_address_, certificate_, whep_session_timeouts{}, video_config_);
    switch (session->startup(std::move(*offer)))
    {
        case whep_session_startup_error::none:
            break;
        case whep_session_startup_error::invalid_offer:
            return {.error = whep_create_error::invalid_offer, .session_id = {}, .answer_sdp = {}};
        case whep_session_startup_error::internal_error:
            return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }

    const auto& session_id = session->id();
    bool inserted = false;
    bool closed = false;
    {
        std::scoped_lock lock(sessions_mutex_);
        closed = closed_;
        if (!closed)
        {
            std::erase_if(sessions_, [](const auto& entry) { return entry.second.expired(); });
            inserted = sessions_.emplace(session_id, session).second;
        }
    }
    if (closed)
    {
        session->shutdown();
        return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }
    if (!inserted)
    {
        spdlog::error("whep session id collision {}", session_id);
        session->shutdown();
        return {.error = whep_create_error::internal_error, .session_id = {}, .answer_sdp = {}};
    }

    spdlog::info("whep session created {} stream {}", session_id, stream_name);
    return {
        .error = whep_create_error::none,
        .session_id = session_id,
        .answer_sdp = session->answer_sdp(),
    };
}

bool whep_service::contains(std::string_view session_id)
{
    std::scoped_lock lock(sessions_mutex_);
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end())
    {
        return false;
    }
    if (iterator->second.expired())
    {
        sessions_.erase(iterator);
        return false;
    }
    return true;
}

bool whep_service::remove(std::string_view session_id)
{
    std::shared_ptr<whep_session> session;
    {
        std::scoped_lock lock(sessions_mutex_);
        const auto iterator = sessions_.find(session_id);
        if (iterator == sessions_.end())
        {
            spdlog::debug("whep session remove not found {}", session_id);
            return false;
        }
        session = iterator->second.lock();
        sessions_.erase(iterator);
    }
    if (!session)
    {
        spdlog::debug("whep session remove expired {}", session_id);
        return false;
    }
    session->shutdown();
    spdlog::info("whep session removed {}", session_id);
    return true;
}

void whep_service::shutdown()
{
    std::map<std::string, std::weak_ptr<whep_session>, std::less<>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        sessions = std::move(sessions_);
        sessions_.clear();
    }
    for (const auto& [id, weak_session] : sessions)
    {
        static_cast<void>(id);
        if (const auto session = weak_session.lock())
        {
            session->shutdown();
        }
    }
}

}    // namespace media_server
