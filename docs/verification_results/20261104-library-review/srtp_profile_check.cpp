#include <array>
#include <deque>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

#include <srtp.h>
#include <crypto_types.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>
#include <spdlog/spdlog.h>

#include "media/webrtc/srtp_transport.h"
#include "media/webrtc/dtls_transport.h"
#include "media/webrtc/dtls_certificate.h"

namespace
{

using media_server::dtls_role;
using media_server::srtp_transport;
using media_server::dtls_transport;
using media_server::dtls_certificate;

int failures = 0;

void expect(bool condition, const std::string& message)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    failures += condition ? 0 : 1;
}

struct reference_profile
{
    const char* name;
    std::size_t key_size;
    std::size_t salt_size;
    void (*rtp_policy)(srtp_crypto_policy_t*);
    void (*rtcp_policy)(srtp_crypto_policy_t*);
};

void aes_cm_sha1_80(srtp_crypto_policy_t* policy)
{
    policy->cipher_type = SRTP_AES_ICM_128;
    policy->cipher_key_len = SRTP_AES_ICM_128_KEY_LEN_WSALT;
    policy->auth_type = SRTP_HMAC_SHA1;
    policy->auth_key_len = 20;
    policy->auth_tag_len = 10;
    policy->sec_serv = sec_serv_conf_and_auth;
}

void aes_gcm_128(srtp_crypto_policy_t* policy)
{
    policy->cipher_type = SRTP_AES_GCM_128;
    policy->cipher_key_len = SRTP_AES_GCM_128_KEY_LEN_WSALT;
    policy->auth_type = SRTP_NULL_AUTH;
    policy->auth_key_len = 0;
    policy->auth_tag_len = 16;
    policy->sec_serv = sec_serv_conf_and_auth;
}

void aes_gcm_256(srtp_crypto_policy_t* policy)
{
    policy->cipher_type = SRTP_AES_GCM_256;
    policy->cipher_key_len = SRTP_AES_GCM_256_KEY_LEN_WSALT;
    policy->auth_type = SRTP_NULL_AUTH;
    policy->auth_key_len = 0;
    policy->auth_tag_len = 16;
    policy->sec_serv = sec_serv_conf_and_auth;
}

constexpr std::array<reference_profile, 3> profiles{{
    {"SRTP_AES128_CM_SHA1_80", 16, 14, aes_cm_sha1_80, aes_cm_sha1_80},
    {"SRTP_AEAD_AES_128_GCM", 16, 12, aes_gcm_128, aes_gcm_128},
    {"SRTP_AEAD_AES_256_GCM", 32, 12, aes_gcm_256, aes_gcm_256},
}};

std::vector<std::uint8_t> drain(BIO* bio)
{
    std::vector<std::uint8_t> output(static_cast<std::size_t>(BIO_ctrl_pending(bio)));
    if (!output.empty())
    {
        BIO_read(bio, output.data(), static_cast<int>(output.size()));
    }
    return output;
}

srtp_t reference_session(const reference_profile& profile, std::vector<std::uint8_t> key, srtp_ssrc_type_t direction)
{
    srtp_policy_t policy{};
    profile.rtp_policy(&policy.rtp);
    profile.rtcp_policy(&policy.rtcp);
    policy.ssrc.type = direction;
    policy.key = key.data();
    policy.window_size = 1024;
    srtp_t session{};
    if (srtp_create(&session, &policy) != srtp_err_status_ok)
    {
        return nullptr;
    }
    return session;
}

std::vector<std::uint8_t> rtp_packet(std::uint32_t ssrc, std::uint16_t sequence)
{
    std::vector<std::uint8_t> packet{0x80, 96, static_cast<std::uint8_t>(sequence >> 8U), static_cast<std::uint8_t>(sequence), 0, 0, 0x10, 0};
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        packet.push_back(static_cast<std::uint8_t>(ssrc >> static_cast<unsigned>(shift)));
    }
    for (std::size_t index = 0; index < 160; ++index)
    {
        packet.push_back(static_cast<std::uint8_t>(index * 7U + sequence));
    }
    return packet;
}

std::vector<std::uint8_t> rtcp_sender_report(std::uint32_t ssrc)
{
    std::vector<std::uint8_t> packet{0x80, 200, 0, 6};
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        packet.push_back(static_cast<std::uint8_t>(ssrc >> static_cast<unsigned>(shift)));
    }
    for (std::size_t index = 0; index < 20; ++index)
    {
        packet.push_back(static_cast<std::uint8_t>(index + 1U));
    }
    return packet;
}

std::vector<std::uint8_t> reference_protect(srtp_t session, std::vector<std::uint8_t> packet, bool rtcp)
{
    auto size = static_cast<int>(packet.size());
    packet.resize(packet.size() + SRTP_MAX_SRTCP_TRAILER_LEN);
    const auto status = rtcp ? srtp_protect_rtcp(session, packet.data(), &size) : srtp_protect(session, packet.data(), &size);
    if (status != srtp_err_status_ok)
    {
        return {};
    }
    packet.resize(static_cast<std::size_t>(size));
    return packet;
}

std::vector<std::uint8_t> reference_unprotect(srtp_t session, std::vector<std::uint8_t> packet, bool rtcp)
{
    auto size = static_cast<int>(packet.size());
    const auto status = rtcp ? srtp_unprotect_rtcp(session, packet.data(), &size) : srtp_unprotect(session, packet.data(), &size);
    if (status != srtp_err_status_ok)
    {
        return {};
    }
    packet.resize(static_cast<std::size_t>(size));
    return packet;
}

void check_profile(const reference_profile& profile)
{
    const auto server_certificate = dtls_certificate::create();
    const auto client_certificate = dtls_certificate::create();

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(DTLS_method()), SSL_CTX_free);
    SSL_CTX_use_certificate(context.get(), client_certificate->certificate());
    SSL_CTX_use_PrivateKey(context.get(), client_certificate->private_key());
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    expect(SSL_CTX_set_tlsext_use_srtp(context.get(), profile.name) == 0, std::string(profile.name) + " client offers only this profile");
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(context.get()), SSL_free);
    BIO* client_input = BIO_new(BIO_s_mem());
    BIO* client_output = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(client_input, -1);
    SSL_set_bio(client.get(), client_input, client_output);
    SSL_set_connect_state(client.get());

    std::deque<std::vector<std::uint8_t>> to_client;
    dtls_transport server("sha-256 " + client_certificate->sha256_fingerprint(),
                          [&to_client](std::span<const std::uint8_t> datagram) { to_client.emplace_back(datagram.begin(), datagram.end()); });
    expect(server.startup(*server_certificate, dtls_role::server), std::string(profile.name) + " server startup");

    bool server_ok = true;
    for (int round = 0; round < 20 && (!server.connected() || SSL_is_init_finished(client.get()) == 0) && server_ok; ++round)
    {
        SSL_do_handshake(client.get());
        if (const auto datagram = drain(client_output); !datagram.empty())
        {
            server_ok = server.handle_datagram(datagram);
        }
        while (!to_client.empty())
        {
            BIO_write(client_input, to_client.front().data(), static_cast<int>(to_client.front().size()));
            to_client.pop_front();
            SSL_do_handshake(client.get());
        }
    }
    const auto* selected = SSL_get_selected_srtp_profile(client.get());
    expect(server_ok && server.connected() && SSL_is_init_finished(client.get()) == 1 && selected != nullptr &&
               std::strcmp(selected->name, profile.name) == 0,
           std::string(profile.name) + " handshake completes with the offered profile");
    if (!server.connected() || selected == nullptr)
    {
        return;
    }

    std::vector<std::uint8_t> material(2U * (profile.key_size + profile.salt_size));
    constexpr char label[] = "EXTRACTOR-dtls_srtp";
    SSL_export_keying_material(client.get(), material.data(), material.size(), label, sizeof(label) - 1U, nullptr, 0, 0);
    const auto client_key = material.begin();
    const auto server_key = client_key + static_cast<std::ptrdiff_t>(profile.key_size);
    const auto client_salt = server_key + static_cast<std::ptrdiff_t>(profile.key_size);
    const auto server_salt = client_salt + static_cast<std::ptrdiff_t>(profile.salt_size);
    std::vector<std::uint8_t> client_master(client_key, server_key);
    client_master.insert(client_master.end(), client_salt, server_salt);
    std::vector<std::uint8_t> server_master(server_key, client_salt);
    server_master.insert(server_master.end(), server_salt, material.end());

    srtp_transport transport;
    expect(transport.startup(*server.srtp_keying_material(), dtls_role::server), std::string(profile.name) + " server srtp startup");
    srtp_t client_outbound = reference_session(profile, client_master, ssrc_any_outbound);
    srtp_t client_inbound = reference_session(profile, server_master, ssrc_any_inbound);
    expect(client_outbound != nullptr && client_inbound != nullptr, std::string(profile.name) + " reference client sessions");

    bool all = true;
    for (std::uint16_t sequence = 1; sequence <= 50; ++sequence)
    {
        const auto down = rtp_packet(0x11223344U, sequence);
        const auto protected_down = transport.protect_rtp(down);
        all = all && protected_down && reference_unprotect(client_inbound, *protected_down, false) == down;

        const auto up = rtp_packet(0x55667788U, sequence);
        const auto unprotected_up = transport.unprotect_rtp(reference_protect(client_outbound, up, false));
        all = all && unprotected_up && *unprotected_up == up;
    }
    expect(all, std::string(profile.name) + " 50 RTP packets each way decrypt to the original");

    const auto report = rtcp_sender_report(0x11223344U);
    const auto protected_report = transport.protect_rtcp(report);
    const auto client_report = rtcp_sender_report(0x55667788U);
    const auto unprotected_report = transport.unprotect_rtcp(reference_protect(client_outbound, client_report, true));
    expect(protected_report && reference_unprotect(client_inbound, *protected_report, true) == report && unprotected_report &&
               *unprotected_report == client_report,
           std::string(profile.name) + " RTCP each way decrypts to the original");

    auto tampered = transport.protect_rtp(rtp_packet(0x11223344U, 51)).value_or(std::vector<std::uint8_t>{});
    if (tampered.size() > 20U)
    {
        tampered[20] ^= 0x01U;
    }
    expect(tampered.size() > 20U && reference_unprotect(client_inbound, tampered, false).empty(),
           std::string(profile.name) + " tampered packet is rejected");

    srtp_dealloc(client_outbound);
    srtp_dealloc(client_inbound);
}

}    // namespace

int main()
{
    spdlog::set_level(spdlog::level::debug);
    for (const auto& profile : profiles)
    {
        check_profile(profile);
    }
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
