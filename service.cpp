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

    io_context_pool workers(config_.threads);
    auto& control_io = workers.context(0);
    stream_registry registry;
    hls_service hls(registry, hls_config{.video = config_.http_video});
    whep_service whep(registry, webrtc_address, config_.whep_video);
    gb28181_service gb28181(registry, &workers);
    if (!whep.ready())
    {
        spdlog::error("dtls certificate create failed");
        return 2;
    }
    auto rtmp = std::make_shared<rtmp_server>(workers, registry, config_.rtmp_port, config_.rtmp_video);
    auto rtsp = std::make_shared<rtsp_server>(workers, registry, config_.rtsp_port, config_.rtsp_video);
    auto http = std::make_shared<http_server>(workers, registry, hls, whep, gb28181, config_.http_port, config_.http_video);

    if (const auto error = rtmp->startup())
    {
        spdlog::error("rtmp listen failed port {} error {}", config_.rtmp_port, error.message());
        return 2;
    }
    if (const auto error = rtsp->startup())
    {
        spdlog::error("rtsp listen failed port {} error {}", config_.rtsp_port, error.message());
        return 2;
    }
    if (const auto error = http->startup())
    {
        spdlog::error("http listen failed port {} error {}", config_.http_port, error.message());
        return 2;
    }

    std::vector<std::weak_ptr<rtsp_pull_session>> pulls;
    for (const auto& [name, url] : config_.rtsp_pulls)
    {
        auto pull = std::make_shared<rtsp_pull_session>(workers.next(), registry, name, url);
        if (!pull->startup())
        {
            spdlog::error("rtsp pull startup failed stream {}", name);
            return 2;
        }
        pulls.push_back(std::move(pull));
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
    signals.async_wait(
        [&](const boost::system::error_code&, int)
        {
            for (const auto& pull : pulls)
            {
                if (const auto session = pull.lock())
                {
                    session->shutdown();
                }
            }
            pulls.clear();
            http->shutdown();
            http.reset();
            whep.shutdown();
            gb28181.shutdown();
            rtsp->shutdown();
            rtsp.reset();
            rtmp->shutdown();
            rtmp.reset();
            workers.release_work();
        });

    spdlog::info("worker threads {}", workers.size());
    workers.run();
    return 0;
}

}    // namespace media_server
