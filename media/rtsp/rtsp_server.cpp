#include <utility>
#include <chrono>

#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

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

void rtsp_server::shutdown(runtime_end_reason reason, std::string error)
{
    std::vector<std::shared_ptr<rtsp_server_connection>> sessions;
    {
        std::scoped_lock lock(mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        for (const auto& weak : sessions_)
        {
            if (const auto session = weak.lock())
            {
                sessions.push_back(session);
            }
        }
        sessions_.clear();
    }
    for (const auto& session : sessions)
    {
        session->shutdown(reason, error);
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
            bool stopped = false;
            {
                std::scoped_lock lock(mutex_);
                stopped = closed_;
            }
            if (!stopped)
            {
                shutdown(runtime_end_reason::runtime_error, error.message());
            }
            return;
        }

        std::scoped_lock lock(mutex_);
        if (closed_)
        {
            boost::system::error_code close_error;
            socket.close(close_error);
            break;
        }

        auto connection = std::make_shared<rtsp_server_connection>(*worker, std::move(socket), config_.rtsp_video.codec,
                                                                   signaling_, std::chrono::milliseconds{60'000},
                                                                   1024U * 1024U, runtime_events_);
        std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
        sessions_.push_back(connection);
        connection->startup();
    }

}

void rtsp_server::safe_shutdown() { listener_.shutdown(); }

}    // namespace media_server
