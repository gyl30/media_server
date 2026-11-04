#include <limits>
#include <exception>
#include <algorithm>

#include "media/net/media_port_pool.h"

namespace media_server
{

namespace
{

constexpr int media_port_start = 49'152;
constexpr int media_port_end = 65'535;

static_assert(media_port_start > 0 && media_port_start % 2 == 0);
static_assert(media_port_start + 1 <= media_port_end && media_port_end <= std::numeric_limits<std::uint16_t>::max());

}    // namespace

std::unique_ptr<media_port_pool> media_port_pool::instance_;

void media_port_pool::init()
{
    if (instance_)
    {
        std::terminate();
    }
    instance_.reset(new media_port_pool());
}

media_port_pool& media_port_pool::instance()
{
    if (!instance_)
    {
        std::terminate();
    }
    return *instance_;
}

media_port_pool::media_port_pool()
{
    constexpr auto count = static_cast<std::size_t>((media_port_end - media_port_start + 1) / 2);
    available_ports_.reserve(count);
    used_ports_.reserve(count);
    for (int port = (media_port_end - 1) & ~1; port >= media_port_start; port -= 2)
    {
        available_ports_.push_back(static_cast<std::uint16_t>(port));
    }
}

std::optional<std::uint16_t> media_port_pool::acquire()
{
    std::scoped_lock lock(mutex_);
    if (available_ports_.empty())
    {
        return std::nullopt;
    }
    const auto port = available_ports_.back();
    available_ports_.pop_back();
    used_ports_.push_back(port);
    return port;
}

void media_port_pool::release(std::uint16_t port)
{
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(used_ports_, port);
    if (found != used_ports_.end())
    {
        used_ports_.erase(found);
        available_ports_.push_back(port);
    }
}

}    // namespace media_server
