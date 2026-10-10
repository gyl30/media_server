#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/ip/address.hpp>

#include "media/webrtc/whip.h"
#include "media/net/worker_context.h"
#include "media/webrtc/whip_session.h"
#include "media/core/stream_registry.h"
#include "media/webrtc/dtls_certificate.h"

namespace media_server::whip
{
namespace
{

struct session_entry
{
    std::string stream_id;
    std::weak_ptr<whip_session> session;
};

struct state
{
    std::mutex mutex;
    std::map<std::string, session_entry, std::less<>> sessions;
    std::set<std::string, std::less<>> streams;
};

state& runtime()
{
    static state value;
    return value;
}

void cleanup_expired(state& current)
{
    for (auto iterator = current.sessions.begin(); iterator != current.sessions.end();)
    {
        if (!iterator->second.session.expired())
        {
            ++iterator;
            continue;
        }
        current.streams.erase(iterator->second.stream_id);
        iterator = current.sessions.erase(iterator);
    }
}

create_result failed(create_error error) { return {.error = error, .session_id = {}, .answer_sdp = {}}; }

void release_stream(std::string_view stream_id)
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    current.streams.erase(std::string(stream_id));
}

}    // namespace

create_result create(worker_context& worker, std::string_view stream_id, std::string_view offer_sdp, const config& application_config)
{
    spdlog::debug("whip create stream {} offer_bytes {}", stream_id, offer_sdp.size());

    if (stream_id.empty() || stream_registry::instance().find(stream_id))
    {
        spdlog::debug("whip create stream conflict {}", stream_id);
        return failed(create_error::stream_conflict);
    }

    auto offer = parse_webrtc_offer(offer_sdp);
    if (!offer)
    {
        spdlog::debug("whip create invalid offer stream {}", stream_id);
        return failed(create_error::invalid_offer);
    }

    boost::system::error_code address_error;
    const auto advertised_address = boost::asio::ip::make_address(application_config.webrtc_address, address_error);
    if (address_error || advertised_address.is_unspecified())
    {
        spdlog::error("whip create invalid advertised address {}", application_config.webrtc_address);
        return failed(create_error::internal_error);
    }

    auto certificate = dtls_certificate::create();
    if (!certificate)
    {
        spdlog::error("whip create dtls certificate failed");
        return failed(create_error::internal_error);
    }

    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        if (!current.streams.emplace(stream_id).second)
        {
            spdlog::debug("whip create stream reserved {}", stream_id);
            return failed(create_error::stream_conflict);
        }
    }

    if (stream_registry::instance().find(stream_id))
    {
        release_stream(stream_id);
        spdlog::debug("whip create stream became unavailable {}", stream_id);
        return failed(create_error::stream_conflict);
    }

    auto session = std::make_shared<whip_session>(worker, std::string(stream_id));
    auto answer_sdp = session->startup(std::move(*offer), advertised_address, std::move(certificate));
    if (!answer_sdp)
    {
        session->shutdown();
        release_stream(stream_id);
        return failed(answer_sdp.error() == whip_session_startup_error::invalid_offer ? create_error::invalid_offer : create_error::internal_error);
    }

    const auto session_id = session->id();
    bool inserted = false;
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        inserted = current.sessions.emplace(session_id, session_entry{.stream_id = std::string(stream_id), .session = session}).second;
        if (!inserted)
        {
            current.streams.erase(std::string(stream_id));
        }
    }
    if (!inserted)
    {
        spdlog::error("whip session id collision {}", session_id);
        session->shutdown();
        return failed(create_error::internal_error);
    }

    spdlog::info("whip session created {} stream {}", session_id, stream_id);
    return {.error = create_error::none, .session_id = session_id, .answer_sdp = std::move(*answer_sdp)};
}

bool remove(std::string_view session_id)
{
    std::shared_ptr<whip_session> session;
    std::string stream_id;
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        const auto iterator = current.sessions.find(session_id);
        if (iterator == current.sessions.end())
        {
            spdlog::debug("whip session remove not found {}", session_id);
            return false;
        }
        session = iterator->second.session.lock();
        stream_id = iterator->second.stream_id;
        current.sessions.erase(iterator);
        current.streams.erase(std::string(stream_id));
    }
    if (!session)
    {
        return false;
    }
    session->shutdown();
    spdlog::info("whip session removed {} stream {}", session_id, stream_id);
    return true;
}

void shutdown()
{
    auto& current = runtime();
    std::scoped_lock lock(current.mutex);
    for (const auto& [id, entry] : current.sessions)
    {
        if (const auto session = entry.session.lock())
        {
            session->shutdown();
        }
    }
    current.sessions.clear();
    current.streams.clear();
}

}    // namespace media_server::whip
