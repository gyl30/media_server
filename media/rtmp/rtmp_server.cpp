#include <chrono>
#include <utility>

#include <boost/asio/detached.hpp>

#include "media/rtmp/rtmp_server.h"
#include "media/rtmp/rtmp_session.h"

namespace media_server
{

rtmp_server::rtmp_server(io_context_pool& workers, const config& config)
    : workers_(workers),
      worker_(workers.next()),
      config_(config),
      listener_(worker_.io(), config.rtmp_port, boost::asio::ip::make_address(config.bind_address))
{
}

void rtmp_server::startup(boost::system::error_code& error)
{
    listener_.startup(error);
    if (error)
    {
        return;
    }

    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void rtmp_server::run(boost::asio::yield_context yield)
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

        auto session =
            std::make_shared<rtmp_session>(*worker, std::move(socket), config_.rtmp_video, std::chrono::milliseconds{15'000}, 1024U * 1024U);
        session->startup();
    }
}

}    // namespace media_server
