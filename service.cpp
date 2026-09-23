#include <chrono>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <utility>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include <boost/uuid/uuid_io.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/uuid/random_generator.hpp>

#include "service.h"
#include "media/core/log.h"
#include "media/http/http_server.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_server.h"
#include "media/net/io_context_pool.h"
#include "media/http/signaling_client.h"

namespace media_server
{

service::service(config cfg) : config_(std::move(cfg)) {}

service::~service() = default;

void service::stop() { workers_->request_stop(); }

bool service::register_signaling(boost::asio::yield_context& yield)
{
    boost::asio::steady_timer retry_timer(yield.get_executor());
    for (;;)
    {
        if (yield.cancelled() != boost::asio::cancellation_type::none)
        {
            return false;
        }
        const auto registration = signaling_client::instance().register_once(yield);
        if (yield.cancelled() != boost::asio::cancellation_type::none)
        {
            return false;
        }
        if (registration.kind == signaling_result_kind::accepted)
        {
            return true;
        }
        if (registration.kind == signaling_result_kind::rejected)
        {
            spdlog::critical("signaling registration rejected status {}; aborting in 5 seconds", registration.status);
            boost::asio::steady_timer abort_timer(yield.get_executor(), std::chrono::seconds{5});
            abort_timer.async_wait(yield);
            if (yield.cancelled() != boost::asio::cancellation_type::none)
            {
                return false;
            }
            std::abort();
        }
        if (registration.kind == signaling_result_kind::temporary_failure)
        {
            spdlog::warn("signaling registration temporary failure status {}; retrying in 1 second", registration.status);
        }
        else
        {
            spdlog::warn("signaling registration network error {}; retrying in 1 second", registration.error);
        }

        retry_timer.expires_after(std::chrono::seconds{1});
        retry_timer.async_wait(yield);
        if (yield.cancelled() != boost::asio::cancellation_type::none)
        {
            return false;
        }
    }
}

void service::run_server(boost::asio::yield_context yield)
{
    if (!register_signaling(yield))
    {
        return;
    }

    boost::scope::scope_exit stop_on_startup_failure([this]() { stop(); });

    boost::system::error_code network_error;
    auto rtmp = std::make_shared<rtmp_server>(*workers_, config_);
    rtmp->startup(network_error);
    if (network_error)
    {
        spdlog::error("rtmp listen failed port {} error {}", config_.rtmp_port, network_error.message());
        return;
    }
    auto rtsp = std::make_shared<rtsp_server>(*workers_, config_);
    rtsp->startup(network_error);
    if (network_error)
    {
        spdlog::error("rtsp listen failed port {} error {}", config_.rtsp_port, network_error.message());
        return;
    }
    auto http = std::make_shared<http_server>(*workers_, config_);
    http->startup(network_error);
    if (network_error)
    {
        spdlog::error("http listen failed port {} error {}", config_.http_port, network_error.message());
        return;
    }
    stop_on_startup_failure.set_active(false);

    spdlog::info("rtmp listen {}:{}", config_.bind_address, config_.rtmp_port);
    spdlog::info("rtsp listen {}:{}", config_.bind_address, config_.rtsp_port);
    spdlog::info("http listen {}:{}", config_.bind_address, config_.http_port);
    spdlog::info("rtmp publish play path app/stream");
    spdlog::info("rtsp play path app/stream");
    spdlog::info("http flv path app/stream.flv");

    signaling_client::instance().run(yield);
}

int service::run()
{
    configure_log_level();

    boost::system::error_code address_error;
    const auto bind_address = boost::asio::ip::make_address(config_.bind_address, address_error);
    if (address_error || bind_address.is_unspecified())
    {
        spdlog::error("invalid bind address {}", config_.bind_address);
        return 1;
    }
    const auto webrtc_address = boost::asio::ip::make_address(config_.webrtc_address, address_error);
    if (address_error || webrtc_address.is_unspecified())
    {
        spdlog::error("invalid webrtc address {}", config_.webrtc_address);
        return 1;
    }

    workers_ = std::make_unique<io_context_pool>(config_.threads);
    auto& control_worker = workers_->context(0);
    auto& control_io = control_worker.io();
    const auto instance_id = boost::uuids::to_string(boost::uuids::random_generator{}());
    signaling_client::instance().configure(config_, instance_id);

    boost::asio::signal_set signals(control_io, SIGINT, SIGTERM);
    control_worker.spawn(
        [&signals](boost::asio::yield_context yield)
        {
            boost::system::error_code error;
            signals.async_wait(yield[error]);
            if (yield.cancelled() == boost::asio::cancellation_type::none && !error)
            {
                std::_Exit(0);
            }
        });

    control_worker.spawn([this](boost::asio::yield_context yield) { run_server(yield); });
    spdlog::info("worker threads {}", workers_->size());
    workers_->run();
    return 0;
}

}    // namespace media_server
