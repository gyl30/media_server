#include <map>
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
#include "media/core/session_registry.h"
#include "media/http/signaling_verify.h"
#include "media/webrtc/dtls_certificate.h"

namespace media_server::whip
{
namespace
{

struct state
{
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<whip_session>, std::less<>> sessions;
};

state& runtime()
{
    static state value;
    return value;
}

void cleanup_expired(state& current)
{
    std::erase_if(current.sessions, [](const auto& entry) { return entry.second.expired(); });
}

create_result failed(create_error error) { return {.error = error, .session_id = {}, .answer_sdp = {}}; }

}    // namespace

create_result create(worker_context& worker,
                     std::string_view stream_id,
                     std::string_view offer_sdp,
                     const config& application_config,
                     const boost::asio::ip::tcp::socket& socket,
                     boost::asio::yield_context yield)
{
    spdlog::debug("whip create stream {} offer_bytes {}", stream_id, offer_sdp.size());

    if (stream_registry::instance().find(stream_id) || session_registry::instance().find_receiver_session(stream_id))
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

    auto session = std::make_shared<whip_session>(worker, std::string(stream_id));
    auto answer_sdp = session->startup(std::move(*offer), advertised_address, std::move(certificate));
    if (!answer_sdp)
    {
        session->shutdown();
        return failed(answer_sdp.error() == whip_session_startup_error::invalid_offer ? create_error::invalid_offer : create_error::internal_error);
    }
    if (!verify_stream(application_config, stream_id, "publish", stream_id, yield) || !socket.is_open())
    {
        session->shutdown();
        return failed(create_error::forbidden);
    }
    if (!session_registry::instance().add_receiver_session(std::string(stream_id), session))
    {
        session->shutdown();
        return failed(create_error::stream_conflict);
    }

    const auto session_id = session->id();
    bool inserted = false;
    {
        auto& current = runtime();
        std::scoped_lock lock(current.mutex);
        cleanup_expired(current);
        inserted = current.sessions.emplace(session_id, session).second;
    }
    if (!inserted)
    {
        spdlog::error("whip session id collision {}", session_id);
        session->shutdown();
        return failed(create_error::internal_error);
    }

    session->activate();
    spdlog::info("whip session created {} stream {}", session_id, stream_id);
    return {.error = create_error::none, .session_id = session_id, .answer_sdp = std::move(*answer_sdp)};
}

bool remove(std::string_view session_id)
{
    std::shared_ptr<whip_session> session;
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
        session = iterator->second.lock();
        current.sessions.erase(iterator);
    }
    if (!session)
    {
        return false;
    }
    session->shutdown();
    spdlog::info("whip session removed {}", session_id);
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
}

}    // namespace media_server::whip
