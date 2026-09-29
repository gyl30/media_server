#include <limits>
#include <vector>
#include <exception>
#include <stdexcept>

#include <boost/asio/error.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/udp_yield_transport.h"

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
    start_port_ = static_cast<std::uint16_t>(start_port);
    end_port_ = static_cast<std::uint16_t>(end_port);
    next_port_ = start_port_;
    next_pair_port_ = start_port_;
    if ((next_pair_port_ & 1U) != 0U)
    {
        ++next_pair_port_;
    }
}

std::optional<std::uint16_t> media_port_pool::reserve()
{
    std::scoped_lock lock(mutex_);
    const auto candidate_count = static_cast<std::uint32_t>(end_port_) - start_port_ + 1U;
    for (std::uint32_t index = 0; index < candidate_count; ++index)
    {
        const auto port = next_port_;
        next_port_ = port == end_port_ ? start_port_ : port + 1U;
        const auto value = static_cast<std::uint16_t>(port);
        if (reserved_.insert(value).second)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<media_port_pool::port_pair> media_port_pool::reserve_pair()
{
    std::scoped_lock lock(mutex_);
    std::uint32_t first_candidate = start_port_;
    if ((first_candidate & 1U) != 0U)
    {
        ++first_candidate;
    }
    if (first_candidate + 1U > end_port_)
    {
        return std::nullopt;
    }
    const auto last_candidate = (static_cast<std::uint32_t>(end_port_) - 1U) & ~1U;
    const auto candidate_count = (last_candidate - first_candidate) / 2U + 1U;
    for (std::uint32_t index = 0; index < candidate_count; ++index)
    {
        const auto first = next_pair_port_;
        next_pair_port_ = first == last_candidate ? first_candidate : first + 2U;
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

std::optional<std::uint16_t> media_port_pool::acquire_and_bind(udp_yield_transport& transport,
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
            error.clear();
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

std::optional<media_port_pool::port_pair> media_port_pool::acquire_pair_and_bind(udp_yield_transport& rtp_transport,
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
            error.clear();
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

void media_port_pool::release(std::uint16_t port)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(port);
}

void media_port_pool::release(port_pair pair)
{
    std::scoped_lock lock(mutex_);
    reserved_.erase(pair.first);
    reserved_.erase(pair.second);
}

}    // namespace media_server
