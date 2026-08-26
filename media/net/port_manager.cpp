#include <limits>
#include <stdexcept>

#include "media/net/port_manager.h"

namespace media_server
{

port_manager_impl::port_manager_impl(int start_port, int end_port)
{
    if (start_port <= 0 || start_port > end_port || end_port > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::invalid_argument("invalid media port range");
    }
    start_port_ = static_cast<std::uint16_t>(start_port);
    end_port_ = static_cast<std::uint16_t>(end_port);
}

std::optional<std::uint16_t> port_manager_impl::acquire()
{
    std::scoped_lock lock(mutex_);
    for (std::uint32_t port = start_port_; port <= end_port_; ++port)
    {
        const auto value = static_cast<std::uint16_t>(port);
        if (reserved_.insert(value).second)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<port_manager_impl::port_pair> port_manager_impl::acquire_pair()
{
    std::scoped_lock lock(mutex_);
    std::uint32_t first = start_port_;
    if ((first & 1U) != 0U)
    {
        ++first;
    }
    for (; first + 1U <= end_port_; first += 2U)
    {
        const auto rtp = static_cast<std::uint16_t>(first);
        const auto rtcp = static_cast<std::uint16_t>(first + 1U);
        if (reserved_.contains(rtp) || reserved_.contains(rtcp))
        {
            continue;
        }
        reserved_.insert(rtp);
        reserved_.insert(rtcp);
        return port_pair{.first = rtp, .second = rtcp};
    }
    return std::nullopt;
}

void port_manager_impl::release(std::uint16_t port)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(port);
}

void port_manager_impl::release(port_pair pair)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(pair.first);
    reserved_.erase(pair.second);
}

}    // namespace media_server
