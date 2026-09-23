#include <utility>

#include <boost/asio/detached.hpp>

#include "media/http/http_server.h"
#include "media/http/http_session.h"

namespace media_server
{
http_server::http_server(io_context_pool& workers, const config& config)
    : workers_(workers),
      worker_(workers.next()),
      config_(config),
      listener_(worker_.io(), config.http_port, boost::asio::ip::make_address(config.bind_address))
{
}

void http_server::startup(boost::system::error_code& error)
{
    listener_.startup(error);
    if (error)
    {
        return;
    }

    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void http_server::run(boost::asio::yield_context yield)
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

        auto session = std::make_shared<http_session>(*worker, std::move(socket), config_);
        session->startup();
    }
}
}    // namespace media_server
