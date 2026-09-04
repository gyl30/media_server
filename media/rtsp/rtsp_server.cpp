#include <utility>

#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include "media/rtsp/rtsp_server.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers, const config& config)
    : workers_(workers),
      worker_(workers.next()),
      config_(config),
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

void rtsp_server::shutdown()
{
    {
        std::scoped_lock lock(mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
    }

    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
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
            break;
        }

        std::scoped_lock lock(mutex_);
        if (closed_)
        {
            boost::system::error_code close_error;
            socket.close(close_error);
            break;
        }

        auto connection = std::make_shared<rtsp_server_connection>(*worker, std::move(socket), config_.rtsp_video.codec);
        connection->startup();
    }

    shutdown();
}

void rtsp_server::safe_shutdown() { listener_.shutdown(); }

}    // namespace media_server
