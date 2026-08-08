#include "media/webrtc/whep_session.h"

#include <openssl/rand.h>

#include <array>
#include <utility>
#include <vector>

namespace media_server
{
namespace
{

std::string random_hex(std::size_t byte_count)
{
    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    {
        return {};
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2U);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

}    // namespace

whep_session::whep_session(
    boost::asio::io_context& io,
    std::shared_ptr<media_stream> stream,
    boost::asio::ip::address advertised_address,
    std::shared_ptr<dtls_certificate> certificate)
    : stream_(std::move(stream)),
      advertised_address_(std::move(advertised_address)),
      certificate_(std::move(certificate)),
      socket_(io)
{
}

bool whep_session::start(webrtc_offer offer)
{
    if (started_ || !stream_ || !certificate_ || stream_->ended())
    {
        return false;
    }

    boost::system::error_code error;
    const auto protocol = advertised_address_.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4();
    socket_.open(protocol, error);
    if (error)
    {
        return false;
    }

    const auto bind_address = advertised_address_.is_v6()
        ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
        : boost::asio::ip::address(boost::asio::ip::address_v4::any());
    socket_.bind(boost::asio::ip::udp::endpoint(bind_address, 0), error);
    if (error)
    {
        close();
        return false;
    }

    const auto endpoint = socket_.local_endpoint(error);
    if (error || endpoint.port() == 0)
    {
        close();
        return false;
    }

    id_ = random_hex(16);
    ice_ufrag_ = random_hex(8);
    ice_pwd_ = random_hex(16);
    if (id_.empty() || ice_ufrag_.empty() || ice_pwd_.empty())
    {
        close();
        return false;
    }

    local_port_ = endpoint.port();
    const auto answer = make_webrtc_answer(
        offer,
        stream_->tracks(),
        webrtc_answer_config{
            .address = advertised_address_,
            .port = local_port_,
            .ice_ufrag = ice_ufrag_,
            .ice_pwd = ice_pwd_,
            .fingerprint = certificate_->sha256_fingerprint(),
        });
    if (!answer)
    {
        close();
        return false;
    }

    offer_ = std::move(offer);
    answer_sdp_ = *answer;
    started_ = true;
    return true;
}

void whep_session::close()
{
    started_ = false;
    answer_sdp_.clear();
    local_port_ = 0;
    boost::system::error_code error;
    socket_.close(error);
}

const std::string& whep_session::id() const noexcept
{
    return id_;
}

const std::string& whep_session::answer_sdp() const noexcept
{
    return answer_sdp_;
}

std::uint16_t whep_session::local_port() const noexcept
{
    return local_port_;
}

}    // namespace media_server
