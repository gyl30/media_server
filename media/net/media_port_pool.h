#ifndef MEDIA_NET_MEDIA_PORT_POOL_H
#define MEDIA_NET_MEDIA_PORT_POOL_H

#include <vector>
#include <mutex>
#include <memory>
#include <cstdint>
#include <optional>

namespace media_server
{

class media_port_pool final
{
   public:
    static void init(int start_port, int end_port);
    [[nodiscard]] static media_port_pool& instance();

    [[nodiscard]] std::optional<std::uint16_t> acquire();
    void release(std::uint16_t port);

   private:
    media_port_pool(int start_port, int end_port);

   private:
    static std::unique_ptr<media_port_pool> instance_;

   private:
    std::mutex mutex_;
    std::vector<std::uint16_t> available_ports_;
    std::vector<std::uint16_t> used_ports_;
};

inline constexpr std::uint16_t default_media_port_start = 49'152;
inline constexpr std::uint16_t default_media_port_end = 65'535;

}    // namespace media_server

#endif
