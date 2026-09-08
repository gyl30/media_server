#ifndef MEDIA_NET_PORT_MANAGER_H
#define MEDIA_NET_PORT_MANAGER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace media_server
{

class port_manager final
{
   public:
    struct port_pair
    {
        std::uint16_t first{};
        std::uint16_t second{};
    };

    static void init(int start_port, int end_port);
    [[nodiscard]] static port_manager& instance();
    static void destroy();

    [[nodiscard]] std::optional<std::uint16_t> acquire();
    [[nodiscard]] std::optional<port_pair> acquire_pair();

    void release(std::uint16_t port);
    void release(port_pair pair);

   private:
    port_manager(int start_port, int end_port);

    static std::unique_ptr<port_manager> instance_;

    std::uint16_t start_port_{};
    std::uint16_t end_port_{};
    std::mutex mutex_;
    std::set<std::uint16_t> reserved_;
};

inline constexpr std::uint16_t default_media_port_start = 49'152;
inline constexpr std::uint16_t default_media_port_end = 65'534;

}    // namespace media_server

#endif
