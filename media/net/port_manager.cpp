#include <limits>
#include <vector>
#include <exception>
#include <stdexcept>

#include <boost/asio/error.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/port_manager.h"
#include "media/net/udp_yield_transport.h"

namespace media_server
{

std::unique_ptr<port_manager> port_manager::instance_;

void port_manager::init(int start_port, int end_port)
{
    if (instance_)
    {
        std::terminate();
    }
    instance_.reset(new port_manager(start_port, end_port));
}

port_manager& port_manager::instance()
{
    if (!instance_)
    {
        std::terminate();
    }
    return *instance_;
}

port_manager::port_manager(int start_port, int end_port)
{
    if (start_port <= 0 || start_port > end_port || end_port > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::invalid_argument("invalid media port range");
    }
    start_port_ = static_cast<std::uint16_t>(start_port);
    end_port_ = static_cast<std::uint16_t>(end_port);
}

std::optional<std::uint16_t> port_manager::reserve()
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

std::optional<port_manager::port_pair> port_manager::reserve_pair()
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

std::optional<std::uint16_t> port_manager::acquire_and_bind(udp_yield_transport& transport,
                                                            const boost::asio::ip::address& bind_address,
                                                            boost::system::error_code& error)
{
    std::vector<std::uint16_t> failed_reservations;
    boost::scope::scope_exit release_failed(
        [&]()
        {
            for (const auto port : failed_reservations)
            {
                release(port);
            }
        });

    error.clear();
    for (;;)
    {
        const auto reserved = reserve();
        if (!reserved)
        {
            return std::nullopt;
        }
        transport.startup(bind_address, *reserved, error);
        if (!error)
        {
            return reserved;
        }
        failed_reservations.push_back(*reserved);
        if (error != boost::asio::error::address_in_use)
        {
            return std::nullopt;
        }
    }
}

std::optional<port_manager::port_pair> port_manager::acquire_pair_and_bind(udp_yield_transport& rtp_transport,
                                                                           udp_yield_transport& rtcp_transport,
                                                                           const boost::asio::ip::address& bind_address,
                                                                           boost::system::error_code& error)
{
    std::vector<port_pair> failed_reservations;
    boost::scope::scope_exit release_failed(
        [&]()
        {
            for (const auto pair : failed_reservations)
            {
                release(pair);
            }
        });

    error.clear();
    for (;;)
    {
        const auto reserved = reserve_pair();
        if (!reserved)
        {
            return std::nullopt;
        }
        rtp_transport.startup(bind_address, reserved->first, error);
        if (!error)
        {
            rtcp_transport.startup(bind_address, reserved->second, error);
        }
        if (!error)
        {
            return reserved;
        }

        rtp_transport.shutdown();
        rtcp_transport.shutdown();
        failed_reservations.push_back(*reserved);
        if (error != boost::asio::error::address_in_use)
        {
            return std::nullopt;
        }
    }
}

void port_manager::release(std::uint16_t port)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(port);
}

void port_manager::release(port_pair pair)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(pair.first);
    reserved_.erase(pair.second);
}

}    // namespace media_server
