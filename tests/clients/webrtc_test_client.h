#ifndef MEDIA_SERVER_TESTS_CLIENTS_WEBRTC_TEST_CLIENT_H
#define MEDIA_SERVER_TESTS_CLIENTS_WEBRTC_TEST_CLIENT_H

#include <span>
#include <atomic>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include <string_view>

#include <boost/asio/io_context.hpp>

namespace media_server::test
{

enum class webrtc_test_direction
{
    publish,
    play,
};

struct webrtc_http_response
{
    unsigned int status{};
    std::string body;
    std::string location;
};

class webrtc_test_context final
{
   public:
    static std::shared_ptr<webrtc_test_context> create();

    ~webrtc_test_context();

    [[nodiscard]] std::string make_offer(webrtc_test_direction direction) const;

   private:
    struct implementation;

    explicit webrtc_test_context(std::unique_ptr<implementation> implementation);

    friend class webrtc_test_peer;
    std::unique_ptr<implementation> implementation_;
};

class webrtc_test_peer final : public std::enable_shared_from_this<webrtc_test_peer>
{
   public:
    using packet_handler = std::function<void(bool rtcp, std::span<const std::uint8_t> packet)>;
    using error_handler = std::function<void(const boost::system::error_code&)>;

    webrtc_test_peer(boost::asio::io_context& io, std::shared_ptr<webrtc_test_context> context);
    ~webrtc_test_peer();

   public:
    bool establish(std::string_view answer_sdp, std::string& error);
    void start_receive(packet_handler packet, error_handler error);
    bool send_rtp(std::span<const std::uint8_t> packet);
    bool send_rtcp(std::span<const std::uint8_t> packet);
    void close() noexcept;

    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] std::uint64_t received_media_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t unprotect_failures() const noexcept;

   private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

[[nodiscard]] bool post_webrtc_offer(
    std::string_view url, std::string_view offer, std::string_view stream_id, webrtc_http_response& response, std::string& error);

[[nodiscard]] bool delete_webrtc_resource(std::string_view url, std::string& error);

}    // namespace media_server::test

#endif
