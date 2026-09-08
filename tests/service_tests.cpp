#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "config.h"
#include "service.h"
#include "media/core/stream_registry.h"

namespace
{

using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::array<std::uint16_t, 3> unused_ports()
{
    boost::asio::io_context io;
    const auto address = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::tcp::acceptor first(io, {address, 0});
    boost::asio::ip::tcp::acceptor second(io, {address, 0});
    boost::asio::ip::tcp::acceptor third(io, {address, 0});
    return {first.local_endpoint().port(), second.local_endpoint().port(), third.local_endpoint().port()};
}

bool can_connect(std::uint16_t port)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), port}, error);
    return !error;
}

bool wait_listening(std::uint16_t port)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (can_connect(port))
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

media_server::config signaling_config(std::string url)
{
    media_server::config cfg;
    cfg.threads = 3;
    const auto ports = unused_ports();
    cfg.rtmp_port = ports[0];
    cfg.rtsp_port = ports[1];
    cfg.http_port = ports[2];
    cfg.signaling_url = std::move(url);
    cfg.server_id = "media-1";
    cfg.control_url = "http://127.0.0.1:" + std::to_string(cfg.http_port);
    cfg.media_ip = "127.0.0.1";
    return cfg;
}

class controlled_registration_server
{
   public:
    controlled_registration_server()
        : acceptor_(io_, {boost::asio::ip::make_address("127.0.0.1"), 0}),
          port_(acceptor_.local_endpoint().port()),
          thread_([this]() { run(); })
    {
    }

    ~controlled_registration_server()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            released_ = true;
        }
        condition_.notify_all();
        boost::asio::ip::tcp::socket wake(io_);
        boost::system::error_code ignored;
        wake.connect({boost::asio::ip::make_address("127.0.0.1"), port_}, ignored);
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    bool wait_request()
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this]() { return requested_; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

   private:
    void run()
    {
        boost::asio::ip::tcp::socket socket(io_);
        boost::system::error_code error;
        acceptor_.accept(socket, error);
        if (error)
        {
            return;
        }
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> request;
        boost::beast::http::read(socket, buffer, request, error);
        if (error)
        {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            requested_ = true;
        }
        condition_.notify_all();
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]() { return released_; });
            if (stopping_)
            {
                return;
            }
        }
        boost::beast::http::response<boost::beast::http::string_body> response{boost::beast::http::status::ok, request.version()};
        response.set(boost::beast::http::field::content_type, "application/json");
        response.body() = R"({"result":"ok"})";
        response.prepare_payload();
        boost::beast::http::write(socket, response, error);
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool requested_{};
    bool released_{};
    bool stopping_{};
    std::thread thread_;
};

void test_signaling_registration_precedes_media_listeners()
{
    controlled_registration_server signaling;
    auto cfg = signaling_config(signaling.url());
    const auto http_port = cfg.http_port;
    media_server::service service(std::move(cfg));
    std::atomic<int> result{-1};
    std::jthread runner([&]() { result.store(service.run()); });

    const bool registration_started = signaling.wait_request();
    const bool listening_before_registration = can_connect(http_port);
    signaling.release();
    const bool listening_after_registration = wait_listening(http_port);
    if (result.load() == -1)
    {
        std::raise(SIGTERM);
    }
    runner.join();

    require(registration_started, "service starts signaling registration");
    require(!listening_before_registration, "media listeners wait for signaling registration");
    require(listening_after_registration, "media listeners start after signaling registration");
    require(result.load() == 0, "service stops cleanly after signaling registration");
}

void test_signal_stops_registration_wait()
{
    controlled_registration_server signaling;
    auto cfg = signaling_config(signaling.url());
    media_server::service service(std::move(cfg));
    std::atomic<int> result{-1};
    std::jthread runner([&]() { result.store(service.run()); });

    require(signaling.wait_request(), "service registration request starts before signal");
    const auto started = std::chrono::steady_clock::now();
    std::raise(SIGTERM);
    runner.join();

    require(std::chrono::steady_clock::now() - started < 500ms, "signal stops in-flight registration");
    require(result.load() == 0, "signal stops service during registration");
}

}    // namespace

int main()
{
    media_server::stream_registry::instance().clear();
    {
        media_server::config cfg;
        cfg.bind_address = "0.0.0.0";
        media_server::service service(std::move(cfg));
        require(service.run() == 1, "service rejects unspecified bind address");
    }

    {
        media_server::config cfg;
        cfg.webrtc_address = "invalid-address";
        media_server::service service(std::move(cfg));
        require(service.run() == 1, "service rejects invalid webrtc address");
    }

    test_signaling_registration_precedes_media_listeners();
    test_signal_stops_registration_wait();

    std::cout << "[pass] service tests\n";
    media_server::stream_registry::instance().clear();
    return 0;
}
