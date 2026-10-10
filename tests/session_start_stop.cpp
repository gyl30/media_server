#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/scope/scope_exit.hpp>

#include "config.h"
#include "media/core/session_registry.h"
#include "media/net/worker_context.h"
#include "media/rtmp/rtmp_session.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void stopped_start(std::string_view protocol, std::string_view phase)
{
    worker_context worker;
    boost::asio::io_context client_io;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor listener(worker.io(), {address, 0});
    tcp::socket peer(client_io);
    peer.connect(listener.local_endpoint());
    tcp::socket socket(worker.io());
    listener.accept(socket);
    listener.close();

    config application_config;
    std::weak_ptr<session> lifetime;
    boost::scope::scope_exit cleanup([&]()
    {
        if (const auto remaining = lifetime.lock())
        {
            remaining->shutdown();
        }
        worker.request_stop();
        worker.io().restart();
        worker.io().run_for(1s);
    });

    if (phase == "before_start")
    {
        worker.request_stop();
    }
    if (protocol == "rtmp")
    {
        auto connection = std::make_shared<rtmp_session>(worker, std::move(socket), application_config);
        lifetime = connection;
        connection->startup();
    }
    else
    {
        auto connection = std::make_shared<rtsp_server_connection>(worker, std::move(socket), application_config);
        lifetime = connection;
        connection->startup();
    }
    if (phase == "active_read")
    {
        require(worker.io().poll() != 0 && !lifetime.expired(), "protocol read did not retain its running session");
    }
    worker.request_stop();
    worker.request_stop();
    worker.io().run_for(1s);
    require(worker.io().stopped(), "worker stop waited for idle timeout after rejected session spawn");
    require(lifetime.expired(), "worker stop retained session through transport callback");
    std::array<std::uint8_t, 1> buffer{};
    boost::system::error_code error;
    peer.non_blocking(true);
    peer.read_some(boost::asio::buffer(buffer), error);
    require(error == boost::asio::error::eof, "stopped protocol session retained its TCP socket");
    std::cout << protocol << " " << phase << " worker stop, session release and peer EOF: PASS\n";
}
}    // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3 || (std::string_view(argv[1]) != "rtmp" && std::string_view(argv[1]) != "rtsp") ||
            (std::string_view(argv[2]) != "before_start" && std::string_view(argv[2]) != "queued_start" &&
             std::string_view(argv[2]) != "active_read"))
        {
            throw std::runtime_error("usage: session_start_stop_tests rtmp|rtsp before_start|queued_start|active_read");
        }
        stopped_start(argv[1], argv[2]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
