#include <csignal>
#include <memory>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include "service.h"

#include "media/core/log.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_service.h"
#include "media/hls/hls_service.h"
#include "media/http/http_server.h"
#include "media/net/io_context_pool.h"
#include "media/rtmp/rtmp_server.h"
#include "media/rtsp/rtsp_pull_session.h"
#include "media/rtsp/rtsp_server.h"
#include "media/webrtc/whep_service.h"

namespace media_server
{

service::service(config cfg) : config_(std::move(cfg)) {}

service::~service() = default;

void service::stop()
{
    for (const auto& pull : pulls_)
    {
        if (const auto session = pull.lock())
        {
            session->shutdown();
        }
    }
    pulls_.clear();

    http_->shutdown();
    http_.reset();
    whep_->shutdown();
    gb28181_->shutdown();
    rtsp_->shutdown();
    rtsp_.reset();
    rtmp_->shutdown();
    rtmp_.reset();
    workers_->release_work();
}

int service::run()
{
    configure_log_level();

    boost::system::error_code address_error;
    const auto webrtc_address = boost::asio::ip::make_address(config_.webrtc_address, address_error);
    if (address_error)
    {
        spdlog::error("invalid webrtc address {}", config_.webrtc_address);
        return 1;
    }

    workers_ = std::make_unique<io_context_pool>(config_.threads);
    auto& control_io = workers_->context(0);
    registry_ = std::make_unique<stream_registry>();
    hls_ = std::make_unique<hls_service>(*registry_, hls_config{.video = config_.http_video});
    whep_ = std::make_unique<whep_service>(*registry_, webrtc_address, config_.whep_video);
    gb28181_ = std::make_unique<gb28181_service>(*registry_, workers_.get());
    if (!whep_->ready())
    {
        spdlog::error("dtls certificate create failed");
        return 2;
    }
    rtmp_ = std::make_shared<rtmp_server>(*workers_, *registry_, config_.rtmp_port, config_.rtmp_video);
    rtsp_ = std::make_shared<rtsp_server>(*workers_, *registry_, config_.rtsp_port, config_.rtsp_video);
    http_ = std::make_shared<http_server>(
        *workers_, *registry_, *hls_, *whep_, *gb28181_, config_.http_port, config_.http_video);

    if (const auto error = rtmp_->startup())
    {
        spdlog::error("rtmp listen failed port {} error {}", config_.rtmp_port, error.message());
        return 2;
    }
    if (const auto error = rtsp_->startup())
    {
        spdlog::error("rtsp listen failed port {} error {}", config_.rtsp_port, error.message());
        return 2;
    }
    if (const auto error = http_->startup())
    {
        spdlog::error("http listen failed port {} error {}", config_.http_port, error.message());
        return 2;
    }

    for (const auto& [name, url] : config_.rtsp_pulls)
    {
        auto pull = std::make_shared<rtsp_pull_session>(workers_->next(), *registry_, name, url);
        if (!pull->startup())
        {
            spdlog::error("rtsp pull startup failed stream {}", name);
            return 2;
        }
        pulls_.push_back(std::move(pull));
    }

    spdlog::info("rtmp listen {}", config_.rtmp_port);
    spdlog::info("rtsp listen {}", config_.rtsp_port);
    spdlog::info("http listen {}", config_.http_port);
    spdlog::info("rtmp publish play path app/stream");
    spdlog::info("rtsp play path app/stream");
    spdlog::info("http flv path app/stream.flv");
    spdlog::info("hls path hls/app/stream/index.m3u8");
    spdlog::info("whep path whep/app/stream");
    spdlog::info("gb28181 path gb28181/app/stream");

    boost::asio::signal_set signals(control_io, SIGINT, SIGTERM);
    signals.async_wait([this](const boost::system::error_code&, int) { stop(); });

    spdlog::info("worker threads {}", workers_->size());
    workers_->run();
    return 0;
}

}    // namespace media_server
