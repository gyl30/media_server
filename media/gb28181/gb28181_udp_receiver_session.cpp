#include <span>
#include <array>
#include <chrono>
#include <limits>
#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>

#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/gb28181/gb28181_udp_receiver_session.h"

namespace media_server
{
namespace
{
constexpr auto rtcp_interval = std::chrono::seconds{1};
}    // namespace

gb28181_udp_receiver_session::gb28181_udp_receiver_session(worker_context& worker,
                                                           std::string stream_id,
                                                           std::uint8_t payload_type,
                                                           std::uint32_t ssrc)
    : worker_(worker), receiver_(worker_, std::move(stream_id), payload_type, ssrc),
      rtp_transport_(std::make_shared<udp_transport>(worker_.io())),
      rtcp_transport_(std::make_shared<udp_transport>(worker_.io())),
      rtcp_timer_(worker_.io()),
      idle_timer_(worker_.io())
{
}

std::optional<std::uint16_t> gb28181_udp_receiver_session::startup(boost::asio::ip::address bind_address)
{
    if (!receiver_.startup())
    {
        return std::nullopt;
    }

    local_port_ = media_port_pool::instance().acquire();
    if (!local_port_)
    {
        return std::nullopt;
    }
    boost::system::error_code network_error;
    rtp_transport_->startup(bind_address, *local_port_, network_error);
    if (!network_error)
    {
        rtcp_transport_->startup(bind_address, static_cast<std::uint16_t>(*local_port_ + 1U), network_error);
    }
    if (network_error)
    {
        return std::nullopt;
    }

    const auto self = shared_from_this();
    rtcp_transport_->set_write_callback(
        [self](boost::system::error_code error, std::size_t)
        {
            if (!self->local_port_)
            {
                return;
            }
            if (error)
            {
                self->shutdown();
                return;
            }
            self->schedule_rtcp();
        });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtp(yield); });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtcp(yield); });
    schedule_rtcp();
    // 设备不发 BYE 就停止推流时（重启、断网）释放接收端口。
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("gb28181 udp receiver idle timeout {}", receiver_.stream_id());
                          shutdown();
                      });

    spdlog::info(
        "gb28181 udp session started stream {} rtp_port {} rtcp_port {}", receiver_.stream_id(), *local_port_, *local_port_ + 1U);
    return local_port_;
}

void gb28181_udp_receiver_session::update_ssrc(std::uint32_t ssrc)
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self, ssrc]() { self->receiver_.set_expected_ssrc(ssrc); });
}

void gb28181_udp_receiver_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_udp_receiver_session::run_rtp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtp_transport_->read(buffer, endpoint, yield, error);
        if (!local_port_)
        {
            return;
        }
        if (error)
        {
            shutdown();
            return;
        }
        idle_timer_.touch();

        const auto result = receiver_.receive_rtp(std::span{buffer.data(), bytes});
        if (result == gb28181_rtp_receive_result::fatal)
        {
            shutdown();
            return;
        }
        if (result == gb28181_rtp_receive_result::accepted && !remote_rtp_endpoint_)
        {
            rtp_transport_->connect(endpoint, error);
            if (error)
            {
                shutdown();
                return;
            }
            remote_rtp_endpoint_ = endpoint;
        }
    }
}

void gb28181_udp_receiver_session::run_rtcp(boost::asio::yield_context yield)
{
    std::vector<std::uint8_t> buffer(64 * 1024);
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        const auto bytes = rtcp_transport_->read(buffer, endpoint, yield, error);
        if (!local_port_)
        {
            return;
        }
        if (error)
        {
            shutdown();
            return;
        }
        if (!receiver_.receive_rtcp(std::span{buffer.data(), bytes}) || remote_rtcp_endpoint_ || !remote_rtp_endpoint_ ||
            endpoint.address() != remote_rtp_endpoint_->address())
        {
            continue;
        }

        rtcp_transport_->connect(endpoint, error);
        if (error)
        {
            shutdown();
            return;
        }
        remote_rtcp_endpoint_ = endpoint;
    }
}

void gb28181_udp_receiver_session::schedule_rtcp()
{
    if (!local_port_)
    {
        return;
    }

    rtcp_timer_.expires_after(rtcp_interval);
    const auto self = shared_from_this();
    rtcp_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || !self->local_port_)
            {
                return;
            }

            std::optional<boost::asio::ip::udp::endpoint> target = self->remote_rtcp_endpoint_;
            if (!target && self->remote_rtp_endpoint_ && self->remote_rtp_endpoint_->port() != std::numeric_limits<std::uint16_t>::max())
            {
                target.emplace(self->remote_rtp_endpoint_->address(), static_cast<std::uint16_t>(self->remote_rtp_endpoint_->port() + 1U));
            }
            if (!target)
            {
                self->schedule_rtcp();
                return;
            }

            std::array<std::uint8_t, 1500> buffer{};
            const auto bytes = self->receiver_.generate_rtcp(buffer);
            if (bytes <= 0)
            {
                self->schedule_rtcp();
                return;
            }

            std::vector<std::uint8_t> packet(buffer.begin(), buffer.begin() + bytes);
            if (!self->rtcp_transport_->write(std::move(packet), *target))
            {
                self->schedule_rtcp();
            }
        });
}

void gb28181_udp_receiver_session::safe_shutdown()
{
    session_registry::instance().remove_receiver_session(receiver_.stream_id(), *this);
    rtcp_timer_.cancel();
    idle_timer_.stop();
    rtp_transport_->shutdown();
    rtcp_transport_->shutdown();
    receiver_.shutdown();
    // 端口在 socket 关闭后才归还，且只归还一次。
    if (local_port_)
    {
        media_port_pool::instance().release(*local_port_);
        local_port_.reset();
    }
    spdlog::debug("gb28181 udp session shutdown {}", receiver_.stream_id());
}

}    // namespace media_server
