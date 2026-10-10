#include <chrono>
#include <cstdint>
#include <memory>
#include <csignal>
#include <utility>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include <boost/scope/scope_exit.hpp>

#include "service.h"
#include "media/core/log.h"
#include "media/core/session_registry.h"
#include "media/net/tcp_listener.h"
#include "media/http/http_session.h"
#include "media/hls/hls.h"
#include "media/webrtc/whip.h"
#include "media/webrtc/whep.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_server_connection.h"
#include "media/net/worker_pool.h"

namespace media_server
{

namespace
{

constexpr std::chrono::seconds accept_retry_interval{3};

template <typename StartSession>
bool start_tcp_listener(worker_pool& workers,
                        boost::asio::ip::address bind_address,
                        std::uint16_t port,
                        StartSession start_session,
                        boost::system::error_code& error)
{
    auto& listener_worker = workers.next();
    auto listener = std::make_shared<tcp_listener>(listener_worker.io(), port, std::move(bind_address));
    listener->startup(error);
    if (error)
    {
        listener->shutdown();
        return false;
    }

    listener_worker.spawn(
        [&workers, listener, port, start_session = std::move(start_session)](boost::asio::yield_context yield) mutable
        {
            boost::system::error_code accept_error;
            for (;;)
            {
                auto& worker = workers.next();
                boost::asio::ip::tcp::socket socket(worker.io());
                listener->accept(socket, yield, accept_error);
                if (yield.cancelled() != boost::asio::cancellation_type::none || accept_error == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (accept_error)
                {
                    // 不区分错误原因：固定间隔重试，监听恢复后继续服务，日志也不会刷屏。
                    spdlog::error("accept failed port {} error {}", port, accept_error.message());
                    boost::asio::steady_timer retry(yield.get_executor(), accept_retry_interval);
                    boost::system::error_code wait_error;
                    retry.async_wait(yield[wait_error]);
                    if (yield.cancelled() != boost::asio::cancellation_type::none)
                    {
                        return;
                    }
                    continue;
                }
                start_session(worker, std::move(socket));
            }
        });
    return true;
}

}    // namespace

service::service(config cfg) : config_(std::move(cfg)) {}

service::~service() = default;

void service::stop()
{
    whip::shutdown();
    whep::shutdown();
    session_registry::instance().shutdown_all();
    workers_->request_stop();
}

bool service::run_server()
{
    boost::scope::scope_exit stop_on_startup_failure([this]() { stop(); });

    boost::system::error_code network_error;
    const auto bind_address = boost::asio::ip::make_address(config_.bind_address);
    if (!start_tcp_listener(
            *workers_,
            bind_address,
            config_.rtmp_port,
            [this](worker_context& worker, boost::asio::ip::tcp::socket socket)
            {
                auto session = std::make_shared<rtmp_session>(worker, std::move(socket), config_);
                session->startup();
            },
            network_error))
    {
        spdlog::error("rtmp listen failed port {} error {}", config_.rtmp_port, network_error.message());
        return false;
    }
    if (!start_tcp_listener(
            *workers_,
            bind_address,
            config_.rtsp_port,
            [this](worker_context& worker, boost::asio::ip::tcp::socket socket)
            {
                auto connection = std::make_shared<rtsp_server_connection>(worker, std::move(socket), config_);
                connection->startup();
            },
            network_error))
    {
        spdlog::error("rtsp listen failed port {} error {}", config_.rtsp_port, network_error.message());
        return false;
    }
    if (!start_tcp_listener(
            *workers_,
            bind_address,
            config_.http_port,
            [this](worker_context& worker, boost::asio::ip::tcp::socket socket)
            {
                auto session = std::make_shared<http_session>(worker, std::move(socket), config_);
                session->startup();
            },
            network_error))
    {
        spdlog::error("http listen failed port {} error {}", config_.http_port, network_error.message());
        return false;
    }
    stop_on_startup_failure.set_active(false);

    spdlog::info("rtmp listen {}:{}", config_.bind_address, config_.rtmp_port);
    spdlog::info("rtsp listen {}:{}", config_.bind_address, config_.rtsp_port);
    spdlog::info("http listen {}:{}", config_.bind_address, config_.http_port);
    spdlog::info("rtmp publish path live/stream_id play path stream_id/token");
    spdlog::info("rtsp publish path stream_id play path stream_id/token");
    spdlog::info("http flv path stream_id/token.flv");
    return true;
}

int service::run()
{
    configure_log_level();

    workers_ = std::make_unique<worker_pool>(config_.threads);
    auto& control_worker = workers_->context(0);
    hls::startup(control_worker);
    auto& control_io = control_worker.io();
    boost::asio::signal_set signals(control_io, SIGINT, SIGTERM);
    control_worker.spawn(
        [this, &signals](boost::asio::yield_context yield)
        {
            boost::system::error_code error;
            signals.async_wait(yield[error]);
            if (yield.cancelled() == boost::asio::cancellation_type::none && !error)
            {
                stop();
            }
        });

    int result = 0;
    control_worker.spawn([this, &result](boost::asio::yield_context) { result = run_server() ? 0 : 1; });
    spdlog::info("worker threads {}", workers_->size());
    workers_->run();
    hls::shutdown();
    return result;
}

}    // namespace media_server
