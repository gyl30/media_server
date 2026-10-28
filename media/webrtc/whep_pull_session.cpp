#include <array>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <cstring>
#include <utility>
#include <charconv>
#include <algorithm>
#include <string_view>

#include <ada.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/worker_pool.h"
#include "media/net/worker_context.h"
#include "media/net/media_port_pool.h"
#include "media/webrtc/stun_message.h"
#include "media/webrtc/whep_pull_session.h"

namespace media_server
{
namespace
{

constexpr auto http_timeout = std::chrono::seconds{5};
constexpr auto establishment_timeout = std::chrono::seconds{15};
constexpr auto check_interval = std::chrono::milliseconds{100};
constexpr auto consent_interval = std::chrono::seconds{5};
constexpr auto consent_timeout = std::chrono::seconds{30};
constexpr auto transaction_timeout = std::chrono::seconds{5};
constexpr std::uint32_t host_candidate_priority = 2'130'706'431U;
constexpr std::size_t max_sdp_size = 64U * 1024U;

bool is_rtcp(std::span<const std::uint8_t> packet) { return packet.size() >= 2U && packet[1] >= 192U && packet[1] <= 223U; }

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

struct http_result
{
    unsigned status{};
    std::string location;
    std::string body;
};

std::optional<http_result> http_exchange(boost::asio::ip::tcp::resolver& resolver,
                                         const ada::url_aggregator& url,
                                         boost::beast::http::verb method,
                                         std::string body,
                                         std::string_view token,
                                         boost::asio::yield_context yield)
{
    auto hostname = url.get_hostname();
    if (hostname.starts_with('[') && hostname.ends_with(']'))
    {
        hostname.remove_prefix(1);
        hostname.remove_suffix(1);
    }
    const std::string port = url.get_port().empty() ? "80" : std::string(url.get_port());
    boost::beast::tcp_stream stream(yield.get_executor());
    boost::asio::steady_timer deadline(yield.get_executor());
    const auto active = std::make_shared<bool>(true);
    const auto expires = std::chrono::steady_clock::now() + http_timeout;
    deadline.expires_at(expires);
    deadline.async_wait(
        [active, &resolver, &stream](boost::system::error_code error)
        {
            if (!error && *active)
            {
                resolver.cancel();
                stream.cancel();
            }
        });
    boost::scope::scope_exit stop_deadline(
        [&]()
        {
            *active = false;
            deadline.cancel();
        });
    boost::system::error_code error;
    const auto endpoints = resolver.async_resolve(std::string(hostname), port, yield[error]);
    if (error)
    {
        return std::nullopt;
    }
    stream.expires_at(expires);
    stream.async_connect(endpoints, yield[error]);
    if (error)
    {
        return std::nullopt;
    }

    std::string target{url.get_pathname()};
    target.append(url.get_search());
    boost::beast::http::request<boost::beast::http::string_body> request(method, target, 11);
    request.set(boost::beast::http::field::host, url.get_host());
    if (!token.empty())
    {
        request.set(boost::beast::http::field::authorization, "Bearer " + std::string(token));
    }
    if (!body.empty())
    {
        request.set(boost::beast::http::field::content_type, "application/sdp");
        request.body() = std::move(body);
    }
    request.keep_alive(false);
    request.prepare_payload();
    boost::beast::http::async_write(stream, request, yield[error]);
    if (error)
    {
        return std::nullopt;
    }
    boost::beast::flat_buffer buffer;
    boost::beast::http::response_parser<boost::beast::http::string_body> response;
    response.body_limit(max_sdp_size);
    boost::beast::http::async_read(stream, buffer, response, yield[error]);
    if (error)
    {
        return std::nullopt;
    }
    return http_result{
        .status = response.get().result_int(),
        .location = std::string(response.get()[boost::beast::http::field::location]),
        .body = std::move(response.get().body()),
    };
}

struct placement
{
    std::mutex mutex;
    worker_pool* workers{};
    std::vector<std::size_t> sessions;
};

placement& placements()
{
    static placement value;
    return value;
}

void delete_resource(worker_context& worker, std::string resource_url, std::string token)
{
    boost::asio::spawn(
        worker.io(),
        [resource_url = std::move(resource_url), token = std::move(token)](boost::asio::yield_context yield)
        {
            const auto url = ada::parse<ada::url_aggregator>(resource_url);
            boost::asio::ip::tcp::resolver resolver(yield.get_executor());
            const auto result = url ? http_exchange(resolver, *url, boost::beast::http::verb::delete_, {}, token, yield) : std::nullopt;
            spdlog::info("whep pull resource delete {} status {}", resource_url, result ? result->status : 0U);
        },
        boost::asio::detached);
}

}    // namespace

whep_pull_session::whep_pull_session(std::size_t worker_index, worker_context& worker, whep_pull_config config)
    : worker_index_(worker_index),
      worker_(worker),
      config_(std::move(config)),
      resolver_(worker_.io()),
      check_timer_(worker_.io()),
      dtls_timer_(worker_.io()),
      establishment_timer_(worker_.io()),
      idle_timer_(worker_.io())
{
}

whep_pull_session::~whep_pull_session()
{
    auto& current = placements();
    std::scoped_lock lock(current.mutex);
    --current.sessions[worker_index_];
}

void whep_pull_session::startup()
{
    const auto self = shared_from_this();
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("whep pull idle timeout {}", config_.stream_id);
                          shutdown();
                      });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void whep_pull_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void whep_pull_session::safe_shutdown()
{
    idle_timer_.stop();
    session_registry::instance().remove_receiver_session(config_.stream_id, *this);
    resolver_.cancel();
    check_timer_.cancel();
    dtls_timer_.cancel();
    establishment_timer_.cancel();
    dtls_.reset();
    srtp_.reset();
    media_receiver_.reset();
    selected_.reset();
    pending_checks_.clear();
    if (udp_transport_)
    {
        udp_transport_->shutdown();
    }
    if (local_port_ != 0)
    {
        media_port_pool::instance().release(local_port_);
        local_port_ = 0;
    }
    if (!resource_url_.empty())
    {
        delete_resource(worker_, std::exchange(resource_url_, {}), config_.token);
    }
    spdlog::debug("whep pull shutdown {}", config_.stream_id);
}

bool whep_pull_session::aborted(const boost::asio::yield_context& yield) const
{
    return yield.cancelled() != boost::asio::cancellation_type::none ||
           session_registry::instance().find_receiver_session(config_.stream_id).get() != this;
}

void whep_pull_session::run(boost::asio::yield_context yield)
{
    if (!negotiate(yield))
    {
        shutdown();
        return;
    }
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context udp_yield) { self->run_udp(udp_yield); });
    send_checks();
    establishment_timer_.expires_after(establishment_timeout);
    establishment_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || self->srtp_ || self->local_port_ == 0)
            {
                return;
            }
            spdlog::info("whep pull establishment timeout {} ice {}", self->config_.stream_id, self->selected_.has_value());
            self->shutdown();
        });
}

bool whep_pull_session::negotiate(boost::asio::yield_context yield)
{
    const auto port = media_port_pool::instance().acquire();
    if (!port)
    {
        spdlog::error("whep pull no available media port {}", config_.stream_id);
        return false;
    }
    local_port_ = *port;
    udp_transport_ = std::make_shared<udp_transport>(worker_.io());
    boost::system::error_code error;
    udp_transport_->startup(config_.local_address, local_port_, udp_receive_buffer::media_input, error);
    certificate_ = dtls_certificate::create();
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (error || !certificate_ || ice_ufrag_.empty() || ice_pwd_.empty() || RAND_bytes(reinterpret_cast<unsigned char*>(&tie_breaker_), 8) != 1 ||
        RAND_bytes(transaction_prefix_.data(), static_cast<int>(transaction_prefix_.size())) != 1)
    {
        spdlog::error("whep pull local transport setup failed {} {}", config_.stream_id, error.message());
        return false;
    }

    const auto offer = make_whep_pull_offer(webrtc_answer_config{
        .address = config_.local_address,
        .port = local_port_,
        .stream_id = config_.stream_id,
        .ice_ufrag = ice_ufrag_,
        .ice_pwd = ice_pwd_,
        .fingerprint = certificate_->sha256_fingerprint(),
    });
    const auto endpoint = ada::parse<ada::url_aggregator>(config_.url);
    const auto result = http_exchange(resolver_, *endpoint, boost::beast::http::verb::post, offer, config_.token, yield);
    if (result && (result->status == 200U || result->status == 201U) && !result->location.empty())
    {
        if (const auto resource = ada::parse<ada::url_aggregator>(result->location, &*endpoint); resource && resource->get_protocol() == "http:")
        {
            resource_url_ = std::string(resource->get_href());
        }
    }
    if (aborted(yield))
    {
        return false;
    }
    if (!result || (result->status != 200U && result->status != 201U))
    {
        spdlog::warn("whep pull offer rejected {} status {}", config_.stream_id, result ? result->status : 0U);
        return false;
    }
    if (resource_url_.empty())
    {
        spdlog::warn("whep pull answer without usable resource location {}", config_.stream_id);
        return false;
    }

    answer_ = parse_whep_pull_answer(result->body);
    if (answer_)
    {
        std::erase_if(answer_->candidates,
                      [this](const boost::asio::ip::udp::endpoint& candidate)
                      { return candidate.address().is_v4() != config_.local_address.is_v4(); });
    }
    if (!answer_ || answer_->candidates.empty() || (answer_->setup != "passive" && answer_->setup != "active") ||
        !dtls_transport::valid_sha256_fingerprint(answer_->fingerprint))
    {
        spdlog::warn("whep pull unsupported answer {}", config_.stream_id);
        return false;
    }
    media_receiver_ = std::make_unique<whip_media_receiver>(worker_, config_.stream_id);
    if (!media_receiver_->startup(whip_media_receiver_config{
            .video_codec = codec_id::h264,
            .video_payload_type = answer_->video_payload_type,
            .audio_payload_type = answer_->audio_payload_type.value_or(-1),
            .audio_channel_count = 2,
        }))
    {
        spdlog::error("whep pull media receiver startup failed {}", config_.stream_id);
        return false;
    }
    const auto self = shared_from_this();
    udp_transport_->set_error_callback(
        [self](boost::system::error_code write_error)
        {
            if (self->local_port_ != 0)
            {
                spdlog::debug("whep pull udp send failed {} {}", self->config_.stream_id, write_error.message());
                self->shutdown();
            }
        });
    spdlog::info("whep pull answer accepted {} candidates {} setup {} audio {}",
                 config_.stream_id,
                 answer_->candidates.size(),
                 answer_->setup,
                 answer_->audio_payload_type.has_value());
    return true;
}

void whep_pull_session::run_udp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::asio::ip::udp::endpoint endpoint;
    boost::system::error_code error;
    for (;;)
    {
        const auto bytes = udp_transport_->read(buffer, endpoint, yield, error);
        if (error)
        {
            break;
        }
        handle_packet(std::span<const std::uint8_t>{buffer.data(), bytes}, endpoint);
    }
    shutdown();
}

void whep_pull_session::handle_packet(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (local_port_ == 0 || packet.empty())
    {
        return;
    }
    if (is_stun_message(packet))
    {
        handle_stun(packet, endpoint);
        return;
    }
    if (!selected_ || endpoint != *selected_)
    {
        return;
    }
    idle_timer_.touch();
    if (dtls_transport::is_dtls_packet(packet))
    {
        handle_dtls(packet);
        return;
    }
    if (srtp_transport::is_rtp_or_rtcp(packet))
    {
        handle_srtp(packet);
    }
}

void whep_pull_session::handle_stun(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (const auto transaction = parse_stun_binding_success_response(packet, answer_->ice_pwd))
    {
        const auto now = std::chrono::steady_clock::now();
        const auto pending = std::find_if(pending_checks_.begin(), pending_checks_.end(),
                                         [&](const pending_check& check)
                                         {
                                             return check.transaction == *transaction && check.endpoint == endpoint &&
                                                    now - check.sent < transaction_timeout;
                                         });
        if (pending == pending_checks_.end())
        {
            return;
        }
        pending_checks_.erase(pending);
        if (selected_)
        {
            if (endpoint == *selected_)
            {
                if (now - consent_received_ >= consent_timeout)
                {
                    shutdown();
                    return;
                }
                idle_timer_.touch();
                consent_received_ = now;
            }
            return;
        }
        selected_ = endpoint;
        consent_received_ = now;
        pending_checks_.clear();
        idle_timer_.touch();
        spdlog::info("whep pull ice connected {} remote {} {}", config_.stream_id, endpoint.address().to_string(), endpoint.port());
        const auto self = shared_from_this();
        dtls_ =
            std::make_unique<dtls_transport>(answer_->fingerprint,
                                             [self](std::span<const std::uint8_t> datagram)
                                             {
                                                 if (self->selected_)
                                                 {
                                                     self->send_udp(datagram, *self->selected_);
                                                 }
                                             });
        if (!dtls_->startup(*certificate_, answer_->setup == "passive" ? dtls_role::client : dtls_role::server))
        {
            spdlog::error("whep pull dtls startup failed {}", config_.stream_id);
            shutdown();
            return;
        }
        schedule_dtls_timeout();
        return;
    }

    const auto request = parse_stun_binding_request(packet, ice_ufrag_ + ":" + answer_->ice_ufrag, ice_pwd_);
    if (!request || !request->unknown_required_attributes.empty())
    {
        return;
    }
    auto response = request->ice_controlling ? make_stun_binding_error_response(*request, 487, "Role Conflict", {}, ice_pwd_)
                                             : make_stun_binding_success_response(*request, endpoint, ice_pwd_);
    send_udp(response, endpoint);
}

void whep_pull_session::handle_dtls(std::span<const std::uint8_t> packet)
{
    if (!dtls_)
    {
        return;
    }
    const bool was_connected = dtls_->connected();
    if (!dtls_->handle_datagram(packet))
    {
        spdlog::info("whep pull dtls closed {}", config_.stream_id);
        shutdown();
        return;
    }
    if (was_connected || !dtls_->connected())
    {
        schedule_dtls_timeout();
        return;
    }

    dtls_timer_.cancel();
    auto srtp = std::make_unique<srtp_transport>();
    if (!srtp->startup(*dtls_->srtp_keying_material(), answer_->setup == "passive" ? dtls_role::client : dtls_role::server))
    {
        spdlog::error("whep pull srtp startup failed {}", config_.stream_id);
        shutdown();
        return;
    }
    srtp_ = std::move(srtp);
    establishment_timer_.cancel();
    spdlog::info("whep pull srtp started {}", config_.stream_id);
}

void whep_pull_session::handle_srtp(std::span<const std::uint8_t> packet)
{
    if (!srtp_)
    {
        return;
    }
    const bool rtcp = is_rtcp(packet);
    auto clear = rtcp ? srtp_->unprotect_rtcp(packet) : srtp_->unprotect_rtp(packet);
    if (!clear)
    {
        spdlog::debug("whep pull srtp rejected {} size {}", config_.stream_id, packet.size());
        return;
    }
    if (!(rtcp ? media_receiver_->input_rtcp(*clear) : media_receiver_->input_rtp(*clear)))
    {
        spdlog::error("whep pull media input failed {} rtcp {}", config_.stream_id, rtcp);
        shutdown();
    }
}

void whep_pull_session::send_checks()
{
    if (local_port_ == 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (selected_ && now - consent_received_ >= consent_timeout)
    {
        spdlog::info("whep pull consent expired {}", config_.stream_id);
        shutdown();
        return;
    }
    std::erase_if(pending_checks_, [&](const pending_check& check) { return now - check.sent >= transaction_timeout; });
    const auto username = answer_->ice_ufrag + ":" + ice_ufrag_;
    const auto& targets = selected_ ? std::vector<boost::asio::ip::udp::endpoint>{*selected_} : answer_->candidates;
    for (const auto& target : targets)
    {
        std::array<std::uint8_t, 12> transaction{};
        std::copy(transaction_prefix_.begin(), transaction_prefix_.end(), transaction.begin());
        const auto sequence = ++check_sequence_;
        std::memcpy(transaction.data() + transaction_prefix_.size(), &sequence, sizeof(sequence));
        pending_checks_.push_back(pending_check{transaction, target, now});
        send_udp(make_stun_binding_request(transaction, username, answer_->ice_pwd, host_candidate_priority, tie_breaker_), target);
    }
    schedule_checks();
}

void whep_pull_session::schedule_checks()
{
    const auto now = std::chrono::steady_clock::now();
    check_timer_.expires_at(selected_ ? std::min(now + consent_interval, consent_received_ + consent_timeout) : now + check_interval);
    const auto self = shared_from_this();
    check_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (!error)
            {
                self->send_checks();
            }
        });
}

void whep_pull_session::schedule_dtls_timeout()
{
    if (!dtls_ || dtls_->connected())
    {
        return;
    }
    const auto timeout = dtls_->timeout();
    if (!timeout)
    {
        return;
    }
    dtls_timer_.expires_after(*timeout);
    const auto self = shared_from_this();
    dtls_timer_.async_wait(
        [self](boost::system::error_code error)
        {
            if (error || !self->dtls_ || self->dtls_->connected())
            {
                return;
            }
            if (!self->dtls_->handle_timeout())
            {
                spdlog::error("whep pull dtls timeout failed {}", self->config_.stream_id);
                self->shutdown();
                return;
            }
            self->schedule_dtls_timeout();
        });
}

void whep_pull_session::send_udp(std::span<const std::uint8_t> packet, const boost::asio::ip::udp::endpoint& endpoint)
{
    if (local_port_ == 0 || packet.empty())
    {
        return;
    }
    if (!udp_transport_->write(packet, endpoint))
    {
        spdlog::warn("whep pull udp write queue full {}", config_.stream_id);
    }
}

namespace whep_pull
{

void startup(worker_pool& workers)
{
    auto& current = placements();
    std::scoped_lock lock(current.mutex);
    current.workers = &workers;
    current.sessions.assign(workers.size(), 0);
}

std::shared_ptr<whep_pull_session> create(whep_pull_config config)
{
    auto& current = placements();
    std::size_t worker_index{};
    {
        std::scoped_lock lock(current.mutex);
        worker_index = static_cast<std::size_t>(std::ranges::min_element(current.sessions) - current.sessions.begin());
        ++current.sessions[worker_index];
    }
    return std::make_shared<whep_pull_session>(worker_index, current.workers->context(worker_index), std::move(config));
}

}    // namespace whep_pull

}    // namespace media_server
