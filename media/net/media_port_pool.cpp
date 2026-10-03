#include <limits>
#include <exception>
#include <stdexcept>
#include <algorithm>

#include "media/net/media_port_pool.h"

namespace media_server
{

std::unique_ptr<media_port_pool> media_port_pool::instance_;

void media_port_pool::init(int start_port, int end_port)
{
    if (instance_)
    {
        std::terminate();
    }
    instance_.reset(new media_port_pool(start_port, end_port));
}

media_port_pool& media_port_pool::instance()
{
    if (!instance_)
    {
        std::terminate();
    }
    return *instance_;
}

media_port_pool::media_port_pool(int start_port, int end_port)
{
    if (start_port <= 0 || start_port > end_port || end_port > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::invalid_argument("invalid media port range");
    }
    start_port += start_port % 2;
    if (start_port + 1 > end_port)
    {
        throw std::invalid_argument("media port range has no complete allocation");
    }
    const auto count = static_cast<std::size_t>((end_port - start_port + 1) / 2);
    available_ports_.reserve(count);
    used_ports_.reserve(count);
    for (int port = (end_port - 1) & ~1; port >= start_port; port -= 2)
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
