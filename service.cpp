#include <memory>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <utility>

#include <boost/asio.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <spdlog/spdlog.h>

#include "service.h"
#include "media/hls/hls.h"
#include "media/core/log.h"
#include "media/http/http_server.h"
#include "media/http/signaling_client.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_server.h"
#include "media/net/io_context_pool.h"
#include "media/core/stream_registry.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace media_server
{

service::service(config cfg) : config_(std::move(cfg)) {}

service::~service() = default;

void service::stop()
{
    if (signaling_abort_timer_)
    {
        signaling_abort_timer_->cancel();
    }
    workers_->stop();
}

void service::schedule_signaling_abort()
{
    if (signaling_abort_timer_)
    {
        return;
    }
    spdlog::critical("signaling fenced this media server instance; aborting in 5 seconds");
    signaling_abort_timer_ = std::make_unique<boost::asio::steady_timer>(workers_->context(0).io());
    signaling_abort_timer_->expires_after(std::chrono::seconds{5});
    signaling_abort_timer_->async_wait(
        [](const boost::system::error_code& error)
        {
            if (!error)
            {
                std::abort();
            }
        });
}

void service::run_control(boost::asio::yield_context yield)
{
    auto& control_io = workers_->context(0).io();
    const auto signaling = signaling_;
    if (signaling)
    {
        boost::asio::steady_timer retry_timer(control_io);
        for (;;)
        {
            const auto registration = signaling->register_once(yield);
            if (registration.kind == signaling_result_kind::accepted)
            {
                break;
            }
            if (registration.kind == signaling_result_kind::rejected)
            {
                spdlog::critical("signaling registration rejected status {}; aborting in 5 seconds", registration.status);
                boost::asio::steady_timer abort_timer(control_io, std::chrono::seconds{5});
                boost::system::error_code error;
                abort_timer.async_wait(yield[error]);
                if (!error)
                {
                    std::abort();
                }
                return;
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
            boost::system::error_code error;
            retry_timer.async_wait(yield[error]);
            if (error)
            {
                return;
            }
        }
    }

    boost::system::error_code network_error;
    rtmp_->startup(network_error);
    if (network_error)
    {
        spdlog::error("rtmp listen failed port {} error {}", config_.rtmp_port, network_error.message());
        exit_code_ = 2;
        stop();
        return;
    }
    rtsp_->startup(network_error);
    if (network_error)
    {
        spdlog::error("rtsp listen failed port {} error {}", config_.rtsp_port, network_error.message());
        exit_code_ = 2;
        stop();
        return;
    }
    http_->startup(network_error);
    if (network_error)
    {
        spdlog::error("http listen failed port {} error {}", config_.http_port, network_error.message());
        exit_code_ = 2;
        stop();
        return;
    }

    for (const auto& [name, url] : config_.rtsp_pulls)
    {
        auto pull = std::make_shared<rtsp_pull_session>(workers_->next(), name, url);
        if (!pull->startup())
        {
            spdlog::error("rtsp pull startup failed stream {}", name);
            exit_code_ = 2;
            stop();
            return;
        }
    }

    spdlog::info("rtmp listen {}:{}", config_.bind_address, config_.rtmp_port);
    spdlog::info("rtsp listen {}:{}", config_.bind_address, config_.rtsp_port);
    spdlog::info("http listen {}:{}", config_.bind_address, config_.http_port);
    spdlog::info("rtmp publish play path app/stream");
    spdlog::info("rtsp play path app/stream");
    spdlog::info("http flv path app/stream.flv");
    spdlog::info("hls path hls/app/stream/index.m3u8");
    spdlog::info("whep path whep/app/stream");
    spdlog::info("gb28181 path gb28181/app/stream");

    if (signaling)
    {
        signaling->run_heartbeat(yield, [this]() { schedule_signaling_abort(); });
    }
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
    auto& control_io = workers_->context(0).io();
    rtmp_ = std::make_shared<rtmp_server>(*workers_, config_);
    rtsp_ = std::make_shared<rtsp_server>(*workers_, config_);
    http_ = std::make_shared<http_server>(*workers_, config_);

    if (!config_.signaling_url.empty())
    {
        signaling_client_options options{
            .signaling_url = config_.signaling_url,
            .server_id = config_.server_id,
            .instance_id = boost::uuids::to_string(boost::uuids::random_generator{}()),
            .control_url = config_.control_url,
            .media_ip = config_.media_ip,
        };
        signaling_ = std::make_shared<signaling_client>(control_io, std::move(options));
    }

    boost::asio::signal_set signals(control_io, SIGINT, SIGTERM);
    signals.async_wait([this](const boost::system::error_code&, int) { stop(); });

    boost::asio::spawn(control_io, [this](boost::asio::yield_context yield) { run_control(yield); }, boost::asio::detached);
    spdlog::info("worker threads {}", workers_->size());
    workers_->run();
    hls::shutdown();
    registry::instance().clear();
    return exit_code_;
}

}    // namespace media_server
