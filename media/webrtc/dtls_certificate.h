#ifndef MEDIA_WEBRTC_DTLS_CERTIFICATE_H
#define MEDIA_WEBRTC_DTLS_CERTIFICATE_H

#include <memory>
#include <string>

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace media_server
{

class dtls_certificate final
{
   public:
    static std::shared_ptr<dtls_certificate> create();

    [[nodiscard]] EVP_PKEY* private_key() const noexcept;
    [[nodiscard]] X509* certificate() const noexcept;
    [[nodiscard]] const std::string& sha256_fingerprint() const noexcept;

   private:
    struct pkey_deleter
    {
        void operator()(EVP_PKEY* value) const noexcept;
    };

    struct x509_deleter
    {
        void operator()(X509* value) const noexcept;
    };

    using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
    using x509_ptr = std::unique_ptr<X509, x509_deleter>;

    dtls_certificate(pkey_ptr private_key, x509_ptr certificate, std::string fingerprint);

    pkey_ptr private_key_;
    x509_ptr certificate_;
    std::string fingerprint_;
};

}    // namespace media_server

#endif
