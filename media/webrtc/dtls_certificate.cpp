#include "media/webrtc/dtls_certificate.h"

#include <openssl/rsa.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <utility>

namespace media_server
{
namespace
{

std::string make_fingerprint(X509* certificate)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (X509_digest(certificate, EVP_sha256(), digest.data(), &digest_size) != 1 || digest_size == 0)
    {
        return {};
    }

    std::string result;
    result.reserve(static_cast<std::size_t>(digest_size) * 3U - 1U);
    std::array<char, 3> byte{};
    for (unsigned int index = 0; index < digest_size; ++index)
    {
        if (index != 0)
        {
            result.push_back(':');
        }
        static_cast<void>(std::snprintf(byte.data(), byte.size(), "%02X", digest[index]));
        result.append(byte.data(), 2);
    }
    return result;
}

}    // namespace

std::shared_ptr<dtls_certificate> dtls_certificate::create()
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free);
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0)
    {
        return {};
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0 || raw_key == nullptr)
    {
        return {};
    }
    pkey_ptr private_key(raw_key);

    x509_ptr certificate(X509_new());
    if (!certificate)
    {
        return {};
    }

    if (X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), static_cast<long>(std::time(nullptr))) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 60L * 60L * 24L * 30L) == nullptr ||
        X509_set_pubkey(certificate.get(), private_key.get()) != 1)
    {
        return {};
    }

    X509_NAME* subject = X509_get_subject_name(certificate.get());
    constexpr unsigned char common_name[] = "media_server";
    if (subject == nullptr || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, common_name, -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 || X509_sign(certificate.get(), private_key.get(), EVP_sha256()) <= 0)
    {
        return {};
    }

    auto fingerprint = make_fingerprint(certificate.get());
    if (fingerprint.empty())
    {
        return {};
    }

    return std::shared_ptr<dtls_certificate>(new dtls_certificate(std::move(private_key), std::move(certificate), std::move(fingerprint)));
}

EVP_PKEY* dtls_certificate::private_key() const noexcept { return private_key_.get(); }

X509* dtls_certificate::certificate() const noexcept { return certificate_.get(); }

const std::string& dtls_certificate::sha256_fingerprint() const noexcept { return fingerprint_; }

void dtls_certificate::pkey_deleter::operator()(EVP_PKEY* value) const noexcept { EVP_PKEY_free(value); }

void dtls_certificate::x509_deleter::operator()(X509* value) const noexcept { X509_free(value); }

dtls_certificate::dtls_certificate(pkey_ptr private_key, x509_ptr certificate, std::string fingerprint)
    : private_key_(std::move(private_key)), certificate_(std::move(certificate)), fingerprint_(std::move(fingerprint))
{
}

}    // namespace media_server
