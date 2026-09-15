#include <utility>
#include <chrono>

#include <boost/asio/detached.hpp>

#include "media/rtsp/rtsp_server.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers,
                         const config& config,
                         std::shared_ptr<signaling_client> signaling,
                         runtime_event_emitter_ptr runtime_events)
    : workers_(workers),
      worker_(workers.next()),
      config_(config),
      signaling_(std::move(signaling)),
      runtime_events_(std::move(runtime_events)),
      listener_(worker_.io(), config.rtsp_port, boost::asio::ip::make_address(config.bind_address))
{
}

void rtsp_server::startup(boost::system::error_code& error)
{
    listener_.startup(error);
    if (error)
    {
        return;
    }

    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void rtsp_server::run(boost::asio::yield_context yield)
{
    boost::system::error_code error;
    for (;;)
    {
        auto* worker = &workers_.next();
        boost::asio::ip::tcp::socket socket(worker->io());
        listener_.accept(socket, {}, yield, error);
        if (error)
        {
            return;
        }

        auto connection = std::make_shared<rtsp_server_connection>(*worker, std::move(socket), config_.rtsp_video.codec,
                                                                   signaling_, std::chrono::milliseconds{60'000},
                                                                   1024U * 1024U, runtime_events_);
        connection->startup();
    }
}

}    // namespace media_server
