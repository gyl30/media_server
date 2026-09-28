#ifndef MEDIA_NET_PORT_MANAGER_H
#define MEDIA_NET_PORT_MANAGER_H

#include <set>
#include <mutex>
#include <memory>
#include <cstdint>
#include <optional>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace media_server
{

class udp_yield_transport;

class port_manager final
{
   public:
    struct port_pair
    {
        std::uint16_t first{};
        std::uint16_t second{};
    };

   public:
    static void init(int start_port, int end_port);
    [[nodiscard]] static port_manager& instance();

    [[nodiscard]] std::optional<std::uint16_t> acquire_and_bind(udp_yield_transport& transport,
                                                                const boost::asio::ip::address& bind_address,
                                                                boost::system::error_code& error);
    [[nodiscard]] std::optional<port_pair> acquire_pair_and_bind(udp_yield_transport& rtp_transport,
                                                                 udp_yield_transport& rtcp_transport,
                                                                 const boost::asio::ip::address& bind_address,
                                                                 boost::system::error_code& error);

    void release(std::uint16_t port);
    void release(port_pair pair);

   private:
    port_manager(int start_port, int end_port);

    [[nodiscard]] std::optional<std::uint16_t> reserve();
    [[nodiscard]] std::optional<port_pair> reserve_pair();

   private:
    static std::unique_ptr<port_manager> instance_;

   private:
    std::uint16_t start_port_{};
    std::uint16_t end_port_{};
    std::uint32_t next_port_{};
    std::uint32_t next_pair_port_{};
    std::mutex mutex_;
    std::set<std::uint16_t> reserved_;
};

inline constexpr std::uint16_t default_media_port_start = 49'152;
inline constexpr std::uint16_t default_media_port_end = 65'534;

}    // namespace media_server

#endif
