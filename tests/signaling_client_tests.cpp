#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "media/http/signaling_client.h"

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

struct captured_request
{
    std::string target;
    std::string body;
};

class test_http_server
{
   public:
    explicit test_http_server(boost::beast::http::status status = boost::beast::http::status::ok,
                              std::string response_body = R"({"result":"ok"})",
                              std::chrono::milliseconds response_delay = {})
        : acceptor_(io_, {boost::asio::ip::make_address("127.0.0.1"), 0}),
          port_(acceptor_.local_endpoint().port()),
          status_(static_cast<unsigned int>(status)),
          response_body_(std::move(response_body)),
          response_delay_(response_delay),
          thread_([this]() { run(); })
    {
    }

    ~test_http_server()
    {
        stopping_.store(true);
        boost::asio::ip::tcp::socket wake(io_);
        boost::system::error_code ignored;
        wake.connect({boost::asio::ip::make_address("127.0.0.1"), port_}, ignored);
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    void set_status(boost::beast::http::status status) { status_.store(static_cast<unsigned int>(status)); }

    std::vector<captured_request> wait_requests(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        require(condition_.wait_for(lock, 2s, [this, count]() { return requests_.size() >= count; }), "HTTP request timeout");
        return requests_;
    }

   private:
    void run()
    {
        for (;;)
        {
            boost::asio::ip::tcp::socket socket(io_);
            boost::system::error_code error;
            acceptor_.accept(socket, error);
            if (error)
            {
                return;
            }
            if (stopping_.load())
            {
                return;
            }
            boost::beast::flat_buffer buffer;
            boost::beast::http::request<boost::beast::http::string_body> request;
            boost::beast::http::read(socket, buffer, request, error);
            if (error)
            {
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back({std::string(request.target()), request.body()});
            }
            condition_.notify_all();
            std::this_thread::sleep_for(response_delay_);
            boost::beast::http::response<boost::beast::http::string_body> response{
                static_cast<boost::beast::http::status>(status_.load()), request.version()};
            response.set(boost::beast::http::field::content_type, "application/json");
            response.body() = response.result_int() >= 200 && response.result_int() < 300 ? response_body_ : R"({"error":"rejected"})";
            response.prepare_payload();
            boost::beast::http::write(socket, response, error);
        }
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::atomic<unsigned int> status_;
    std::string response_body_;
    std::chrono::milliseconds response_delay_;
    std::atomic_bool stopping_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<captured_request> requests_;
    std::thread thread_;
};

media_server::signaling_client_options client_options(std::string url)
{
    return {
        .signaling_url = std::move(url),
        .server_id = "media-1",
        .instance_id = "instance-a",
        .control_url = "http://127.0.0.1:8080",
        .media_ip = "127.0.0.1",
        .heartbeat_interval = 20ms,
        .request_timeout = 500ms,
    };
}

std::uint16_t unused_port()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

void test_registration_and_heartbeat_body()
{
    test_http_server server;
    media_server::signaling_client client(client_options(server.url()));
    require(client.register_once().kind == media_server::signaling_result_kind::accepted, "registration accepted");
    require(client.heartbeat_once().kind == media_server::signaling_result_kind::accepted, "heartbeat accepted");
    const auto requests = server.wait_requests(2);
    require(requests[0].target == "/internal/media-servers/register", "registration target");
    require(requests[1].target == "/internal/media-servers/heartbeat", "heartbeat target");
    const auto registration = boost::json::parse(requests[0].body).as_object();
    const auto heartbeat = boost::json::parse(requests[1].body).as_object();
    require(registration.at("server_id") == "media-1", "registration server id");
    require(registration.at("instance_id") == "instance-a", "registration instance id");
    require(registration.at("control_url") == "http://127.0.0.1:8080", "registration control url");
    require(registration.at("media_ip") == "127.0.0.1", "registration media ip");
    require(heartbeat.at("server_id") == "media-1" && heartbeat.at("instance_id") == "instance-a", "heartbeat identity stable");
}

void test_result_classification()
{
    test_http_server server(boost::beast::http::status::conflict);
    media_server::signaling_client rejected(client_options(server.url()));
    const auto rejected_result = rejected.register_once();
    require(rejected_result.kind == media_server::signaling_result_kind::rejected && rejected_result.status == 409, "registration rejected");

    test_http_server temporary(boost::beast::http::status::internal_server_error);
    media_server::signaling_client retryable(client_options(temporary.url()));
    const auto temporary_result = retryable.register_once();
    require(temporary_result.kind == media_server::signaling_result_kind::temporary_failure && temporary_result.status == 500,
            "server failure is temporary");

    auto options = client_options("http://127.0.0.1:" + std::to_string(unused_port()));
    media_server::signaling_client unavailable(std::move(options));
    require(unavailable.register_once().kind == media_server::signaling_result_kind::network_error, "registration network error");
    require(unavailable.heartbeat_once().kind == media_server::signaling_result_kind::network_error, "heartbeat network error");
}

void test_success_requires_result_ok()
{
    for (const std::string body : {R"({"result":"not-ok"})", R"({})", "{invalid"})
    {
        test_http_server server(boost::beast::http::status::ok, body);
        media_server::signaling_client client(client_options(server.url()));
        const auto result = client.register_once();
        require(result.kind == media_server::signaling_result_kind::rejected && result.status == 200,
                "invalid success body rejected");
    }
}

void test_constructor_rejects_non_base_urls()
{
    for (const std::string url : {
             "http://user:pass@127.0.0.1:8080",
             "http://127.0.0.1:8080/base",
             "http://127.0.0.1:8080?tenant=x",
             "http://127.0.0.1:8080#fragment",
         })
    {
        bool rejected = false;
        try
        {
            media_server::signaling_client client(client_options(url));
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "non-base signaling URL rejected");
    }
}

void test_heartbeat_rejection_and_shutdown()
{
    test_http_server server(boost::beast::http::status::internal_server_error);
    media_server::signaling_client client(client_options(server.url()));
    std::mutex mutex;
    std::condition_variable condition;
    bool fenced = false;
    client.startup_heartbeat(
        [&]()
        {
            std::lock_guard lock(mutex);
            fenced = true;
            condition.notify_all();
        });
    server.wait_requests(1);
    {
        std::lock_guard lock(mutex);
        require(!fenced, "temporary heartbeat failure does not fence instance");
    }
    server.set_status(boost::beast::http::status::gone);
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 2s, [&]() { return fenced; }), "heartbeat rejection callback");
    }
    client.shutdown();

    auto options = client_options(server.url());
    options.heartbeat_interval = 1h;
    media_server::signaling_client sleeping(std::move(options));
    sleeping.startup_heartbeat([]() {});
    const auto started = std::chrono::steady_clock::now();
    sleeping.shutdown();
    require(std::chrono::steady_clock::now() - started < 500ms, "heartbeat shutdown cancels wait");
}

void test_shutdown_cancels_in_flight_heartbeat()
{
    test_http_server server(boost::beast::http::status::ok, R"({"result":"ok"})", 2s);
    auto options = client_options(server.url());
    options.heartbeat_interval = 1ms;
    options.request_timeout = 5s;
    media_server::signaling_client client(std::move(options));
    client.startup_heartbeat([]() {});
    server.wait_requests(1);
    const auto started = std::chrono::steady_clock::now();
    client.shutdown();
    require(std::chrono::steady_clock::now() - started < 500ms, "heartbeat shutdown cancels in-flight request");
}

}    // namespace

int main()
{
    test_registration_and_heartbeat_body();
    test_result_classification();
    test_success_requires_result_ok();
    test_constructor_rejects_non_base_urls();
    test_heartbeat_rejection_and_shutdown();
    test_shutdown_cancels_in_flight_heartbeat();
    return 0;
}
