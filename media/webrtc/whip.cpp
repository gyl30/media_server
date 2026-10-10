#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <boost/asio/ip/address.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/webrtc/whip.h"
#include "media/net/worker_context.h"
#include "media/net/media_port_pool.h"
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

std::string random_hex(std::size_t byte_count)
{
    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    {
        return {};
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

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

    const auto port = media_port_pool::instance().acquire();
    if (!port)
    {
        spdlog::error("webrtc udp socket startup failed: no available media port");
        return failed(create_error::internal_error);
    }
    boost::scope::scope_exit release_port([port]() { media_port_pool::instance().release(*port); });
    auto transport = std::make_shared<udp_transport>(worker.io());
    boost::system::error_code udp_error;
    transport->startup(advertised_address, *port, udp_error);
    if (udp_error)
    {
        spdlog::error("webrtc udp socket startup failed error {}", udp_error.message());
        return failed(create_error::internal_error);
    }
    webrtc_answer_config answer_config{
        .address = advertised_address,
        .port = *port,
        .stream_id = random_hex(16),
        .ice_ufrag = random_hex(8),
        .ice_pwd = random_hex(16),
        .fingerprint = certificate->sha256_fingerprint(),
    };
    if (answer_config.stream_id.empty() || answer_config.ice_ufrag.empty() || answer_config.ice_pwd.empty())
    {
        spdlog::error("webrtc session identifiers create failed");
        return failed(create_error::internal_error);
    }
    auto answer = make_whip_answer(*offer, answer_config);
    if (!answer)
    {
        return failed(create_error::invalid_offer);
    }
    const auto media = std::find_if(offer->media.begin(), offer->media.end(),
                                  [&answer](const webrtc_media_offer& value) { return value.mid == answer->transport_mid; });
    if (media == offer->media.end() || media->ice_ufrag.empty() || media->ice_pwd.empty() ||
        !dtls_transport::valid_sha256_fingerprint(media->fingerprint))
    {
        return failed(create_error::invalid_offer);
    }
    if (!verify_stream(application_config, stream_id, "publish", stream_id, yield) || !socket.is_open())
    {
        return failed(create_error::forbidden);
    }

    auto session = std::make_shared<whip_session>(worker, std::string(stream_id), std::move(transport), std::move(answer_config));
    release_port.set_active(false);
    if (!session->startup(*media, *answer, *certificate))
    {
        session->shutdown();
        return failed(create_error::internal_error);
    }
    // startup 的 UDP 接收在 async receive 处挂起；登记前不能再 yield。
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

    spdlog::info("whip session created {} stream {}", session_id, stream_id);
    return {.error = create_error::none, .session_id = session_id, .answer_sdp = std::move(answer->sdp)};
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
