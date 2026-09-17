#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/ip/address.hpp>

#include "media/webrtc/whep.h"
#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/webrtc/whep_session.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/webrtc/dtls_certificate.h"

namespace media_server::whep
{
namespace
{

struct state
{
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<whep_session>, std::less<>> sessions;
    std::map<std::string, std::weak_ptr<whep_session>, std::less<>> stream_sessions;
};

state& runtime()
{
    static state value;
    return value;
}

create_result failed(create_error error) { return {.error = error, .session_id = {}, .answer_sdp = {}}; }

void cleanup_expired(state& current)
{
    std::erase_if(current.sessions, [](const auto& entry) { return entry.second.expired(); });
    std::erase_if(current.stream_sessions, [](const auto& entry) { return entry.second.expired(); });
}

void release_stream_session(state& current, const whep_session& expected)
{
    const auto iterator = current.stream_sessions.find(expected.stream_id());
    if (iterator == current.stream_sessions.end())
    {
        return;
    }
    const auto session = iterator->second.lock();
    if (!session || session.get() == &expected)
    {
        current.stream_sessions.erase(iterator);
    }
}

void release_stream_session(const std::shared_ptr<whep_session>& expected)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    release_stream_session(current, *expected);
}

}    // namespace

create_result create(
    worker_context& worker, std::string stream_id, std::string_view stream_name, std::string_view offer_sdp, const config& application_config)
{
    spdlog::debug("whep create stream {} offer_bytes {}", stream_name, offer_sdp.size());

    auto stream = stream_registry::instance().find(stream_name);
    if (!stream)
    {
        spdlog::debug("whep create stream not found {}", stream_name);
        return failed(create_error::stream_not_found);
    }

    auto offer = parse_webrtc_offer(offer_sdp);
    if (!offer)
    {
        spdlog::debug("whep create invalid offer stream {}", stream_name);
        return failed(create_error::invalid_offer);
    }

    boost::system::error_code address_error;
    const auto advertised_address = boost::asio::ip::make_address(application_config.webrtc_address, address_error);
    if (address_error || advertised_address.is_unspecified())
    {
        spdlog::error("whep create invalid advertised address {}", application_config.webrtc_address);
        return failed(create_error::internal_error);
    }

    auto certificate = dtls_certificate::create();
    if (!certificate)
    {
        spdlog::error("whep create dtls certificate failed");
        return failed(create_error::internal_error);
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

    auto session = std::make_shared<whep_session>(worker,
                                                  std::move(stream_id),
                                                  stream,
                                                  advertised_address,
                                                  std::move(certificate),
                                                  whep_session_timeouts{},
                                                  application_config.whep_video,
                                                  1024U * 1024U);
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        if (!current.stream_sessions.emplace(session->stream_id(), session).second)
        {
            spdlog::debug("whep create stream id already active {}", session->stream_id());
            return failed(create_error::stream_id_conflict);
        }
    }
    switch (session->startup(std::move(*offer)))
    {
        case whep_session_startup_error::none:
            break;
        case whep_session_startup_error::invalid_offer:
            release_stream_session(session);
            return failed(create_error::invalid_offer);
        case whep_session_startup_error::internal_error:
            release_stream_session(session);
            return failed(create_error::internal_error);
    }

    const auto& session_id = session->id();
    bool inserted = false;
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        inserted = current.sessions.emplace(session_id, session).second;
        if (!inserted)
        {
            release_stream_session(current, *session);
        }
    }
    if (!inserted)
    {
        spdlog::error("whep session id collision {}", session_id);
        signaling_client::instance().report(make_event(event_kind::output,
                                                       event_protocol::whep,
                                                       event_state::runtime_error,
                                                       session->stream_id(),
                                                       session->stream_name(),
                                                       {},
                                                       {},
                                                       "session_id_collision"));
        session->shutdown();
        return failed(create_error::internal_error);
    }

    spdlog::info("whep session created {} stream {}", session_id, stream_name);
    return {.error = create_error::none, .session_id = session_id, .answer_sdp = session->answer_sdp()};
}

bool contains(std::string_view session_id)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    cleanup_expired(current);
    const auto iterator = current.sessions.find(session_id);
    if (iterator == current.sessions.end())
    {
        return false;
    }
    return true;
}

bool remove(std::string_view session_id)
{
    std::shared_ptr<whep_session> session;
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        const auto iterator = current.sessions.find(session_id);
        if (iterator == current.sessions.end())
        {
            spdlog::debug("whep session remove not found {}", session_id);
            return false;
        }
        session = iterator->second.lock();
        current.sessions.erase(iterator);
        if (session)
        {
            release_stream_session(current, *session);
        }
    }
    if (!session)
    {
        spdlog::debug("whep session remove expired {}", session_id);
        return false;
    }
    signaling_client::instance().report(
        make_event(event_kind::output, event_protocol::whep, event_state::stop_requested, session->stream_id(), session->stream_name()));
    session->shutdown();
    spdlog::info("whep session removed {}", session_id);
    return true;
}

void shutdown()
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    for (const auto& [id, entry] : current.sessions)
    {
        if (const auto session = entry.lock())
        {
            session->shutdown();
        }
    }
    current.sessions.clear();
    current.stream_sessions.clear();
}

}    // namespace media_server::whep
