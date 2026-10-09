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
                                                           std::string stream_name,
                                                           std::uint8_t payload_type,
                                                           std::uint32_t ssrc)
    : worker_(worker), receiver_(worker_, std::move(stream_name), payload_type, ssrc, [this]() { idle_timer_.touch(); }),
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

    const auto local_port = media_port_pool::instance().acquire();
    if (!local_port)
    {
        receiver_.shutdown();
        return std::nullopt;
    }
    boost::system::error_code network_error;
    rtp_transport_->startup(bind_address, *local_port, network_error);
    if (!network_error)
    {
        rtcp_transport_->startup(bind_address, static_cast<std::uint16_t>(*local_port + 1U), network_error);
    }
    if (network_error)
    {
        rtp_transport_->shutdown();
        rtcp_transport_->shutdown();
        media_port_pool::instance().release(*local_port);
        receiver_.shutdown();
        return std::nullopt;
    }
    local_port_ = *local_port;

    const auto self = shared_from_this();
    rtcp_transport_->set_write_callback(
        [weak = weak_from_this()](boost::system::error_code error, std::size_t)
        {
            const auto locked = weak.lock();
            if (!locked || !locked->local_port_)
            {
                return;
            }
            if (error)
            {
                locked->shutdown();
                return;
            }
            locked->schedule_rtcp();
        });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtp(yield); });
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtcp(yield); });
    schedule_rtcp();
    // 设备不发 BYE 就停止推流时（重启、断网）释放接收端口。
    idle_timer_.start(self, media_idle_timeout, [weak = weak_from_this()]()
                      {
                          if (const auto owner = weak.lock())
                          {
                              spdlog::info("gb28181 udp receiver idle timeout {}", owner->receiver_.stream_name());
                              owner->shutdown();
                          }
                      });

    spdlog::info(
        "gb28181 udp session started stream {} rtp_port {} rtcp_port {}", receiver_.stream_name(), *local_port_, *local_port_ + 1U);
    return local_port_;
}

void gb28181_udp_receiver_session::update_ssrc(std::uint32_t ssrc, std::function<void(bool)> done)
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(),
                      [self, ssrc, done = std::move(done)]()
                      {
                          const bool open =
                              self->receiver_.running() && session_registry::instance().receiver_open(self->receiver_.stream_name(), *self);
                          if (open)
                          {
                              self->receiver_.set_expected_ssrc(ssrc);
                          }
                          done(open);
                      });
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
    if (!local_port_)
    {
        return;
    }
    const auto local_port = *local_port_;
    local_port_.reset();
    rtcp_timer_.cancel();
    idle_timer_.stop();
    rtp_transport_->shutdown();
    rtcp_transport_->shutdown();
    receiver_.shutdown();
    media_port_pool::instance().release(local_port);
    // 流已移除、socket 已关闭、端口已归还后才移除槽位，删除请求据此确认关闭完成。
    session_registry::instance().remove_receiver_session(receiver_.stream_name(), *this);
    spdlog::debug("gb28181 udp session shutdown {}", receiver_.stream_name());
}

}    // namespace media_server
