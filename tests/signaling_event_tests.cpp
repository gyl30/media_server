#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <string_view>
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/beast.hpp>

#include "media/core/runtime_event.h"
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

enum class response_action
{
    no_content,
    server_error,
    disconnect,
    hold_server_error,
};

struct captured_request
{
    std::string target;
    std::string body;
};

class scripted_http_server
{
   public:
    explicit scripted_http_server(std::vector<response_action> actions = {})
        : acceptor_(io_, {boost::asio::ip::address_v4::loopback(), 0}),
          port_(acceptor_.local_endpoint().port()),
          actions_(std::move(actions)),
          thread_([this]() { run(); })
    {
    }

    ~scripted_http_server()
    {
        stopping_.store(true);
        release_hold();
        boost::asio::ip::tcp::socket wake(io_);
        boost::system::error_code ignored;
        wake.connect({boost::asio::ip::address_v4::loopback(), port_}, ignored);
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    [[nodiscard]] bool wait_requests(std::size_t count, std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count]() { return requests_.size() >= count; });
    }

    [[nodiscard]] std::vector<captured_request> requests() const
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    void release_hold()
    {
        {
            std::lock_guard lock(mutex_);
            hold_released_ = true;
        }
        condition_.notify_all();
    }

   private:
    void run()
    {
        std::size_t request_index{};
        while (!stopping_.load())
        {
            boost::asio::ip::tcp::socket socket(io_);
            boost::system::error_code error;
            acceptor_.accept(socket, error);
            if (error || stopping_.load())
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

            const auto action = request_index < actions_.size() ? actions_[request_index] : response_action::no_content;
            ++request_index;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back({std::string(request.target()), request.body()});
            }
            condition_.notify_all();

            if (action == response_action::hold_server_error)
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return hold_released_ || stopping_.load(); });
                if (stopping_.load())
                {
                    return;
                }
            }
            if (action == response_action::disconnect)
            {
                continue;
            }

            const auto status = action == response_action::server_error || action == response_action::hold_server_error
                                    ? boost::beast::http::status::internal_server_error
                                    : boost::beast::http::status::no_content;
            boost::beast::http::response<boost::beast::http::string_body> response{status, request.version()};
            response.prepare_payload();
            boost::beast::http::write(socket, response, error);
        }
    }

   private:
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::vector<response_action> actions_;
    std::atomic_bool stopping_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<captured_request> requests_;
    bool hold_released_{};
    std::thread thread_;
};

std::string stream_id(std::size_t value)
{
    std::ostringstream output;
    output << "00000000-0000-4000-8000-" << std::setfill('0') << std::setw(12) << value;
    return output.str();
}

media_server::runtime_event event(std::size_t value)
{
    return {
        .kind = media_server::runtime_kind::source,
        .stream_id = stream_id(value),
        .stream_name = "live/camera-" + std::to_string(value),
        .source_id = "10000000-0000-4000-8000-000000000001",
        .protocol = media_server::runtime_protocol::rtsp,
        .state = media_server::runtime_state::starting,
        .stage = "connecting",
    };
}

media_server::signaling_client_options client_options(std::string url)
{
    return {
        .signaling_url = std::move(url),
        .server_id = "media-1",
        .instance_id = "instance-a",
        .control_url = "http://127.0.0.1:8080",
        .media_ip = "127.0.0.1",
        .rtmp_port = 1935,
        .rtsp_port = 8554,
        .http_port = 8080,
        .heartbeat_interval = 20ms,
        .request_timeout = 500ms,
    };
}

class client_fixture
{
   public:
    explicit client_fixture(std::string url) : work_(boost::asio::make_work_guard(io_))
    {
        media_server::signaling_client::instance().configure(client_options(std::move(url)));
    }

    ~client_fixture() { stop(); }

    void start()
    {
        boost::asio::spawn(
            io_,
            [this](boost::asio::yield_context yield) { media_server::signaling_client::instance().run(yield, [this]() { fenced_ = true; }); },
            boost::asio::detached);
        runner_ = std::jthread([this]() { io_.run(); });
    }

    void stop()
    {
        io_.stop();
        if (runner_.joinable())
        {
            runner_.join();
        }
    }

    [[nodiscard]] bool fenced() const noexcept { return fenced_; }

   private:
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::jthread runner_;
    bool fenced_{};
};

boost::json::array batch_events(const captured_request& request) { return boost::json::parse(request.body).as_object().at("events").as_array(); }

void test_unconfigured_report_is_noop()
{
    media_server::signaling_client::instance().report(event(1));
    scripted_http_server server;
    client_fixture fixture(server.url());
    fixture.start();
    require(server.wait_requests(2), "unconfigured report heartbeat requests");
    fixture.stop();
    for (const auto& request : server.requests())
    {
        require(request.target == "/internal/media-servers/heartbeat", "unconfigured report does not survive configuration");
    }
}

void test_heartbeat_failure_retains_events()
{
    scripted_http_server server({response_action::server_error, response_action::no_content, response_action::no_content});
    client_fixture fixture(server.url());
    media_server::signaling_client::instance().report(event(1));
    fixture.start();
    require(server.wait_requests(3), "heartbeat recovery flushes event");
    fixture.stop();
    const auto requests = server.requests();
    require(requests[0].target == "/internal/media-servers/heartbeat" && requests[1].target == "/internal/media-servers/heartbeat",
            "failed heartbeat skips event upload");
    require(requests[2].target == "/internal/runtime-events" && batch_events(requests[2]).size() == 1U, "accepted heartbeat uploads retained event");
}

void test_batch_order_and_identity()
{
    scripted_http_server server;
    client_fixture fixture(server.url());
    auto first = event(1);
    first.state = media_server::runtime_state::stopped;
    first.end_reason = media_server::runtime_end_reason::runtime_error;
    first.error = "connection_failed";
    media_server::signaling_client::instance().report(std::move(first));
    media_server::signaling_client::instance().report(event(2));
    media_server::signaling_client::instance().report(event(3));
    fixture.start();
    require(server.wait_requests(2), "event batch uploaded");
    fixture.stop();

    const auto request = server.requests()[1];
    require(request.target == "/internal/runtime-events", "runtime event batch target");
    const auto body = boost::json::parse(request.body).as_object();
    require(body.size() == 3U && body.at("server_id") == "media-1" && body.at("instance_id") == "instance-a", "batch envelope owns server identity");
    const auto& events = body.at("events").as_array();
    require(events.size() == 3U, "one request contains all events");
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        const auto& value = events[index].as_object();
        require(value.at("stream_id").as_string() == stream_id(index + 1U), "batch preserves event order");
        require(!value.contains("server_id") && !value.contains("instance_id"), "event does not duplicate server identity");
    }
    const auto& terminal = events.front().as_object();
    require(terminal.at("end_reason") == "runtime_error" && terminal.at("error") == "connection_failed", "terminal event fields serialized");
}

void test_failed_batch_is_retried()
{
    scripted_http_server server(
        {response_action::no_content, response_action::server_error, response_action::no_content, response_action::no_content});
    client_fixture fixture(server.url());
    media_server::signaling_client::instance().report(event(1));
    media_server::signaling_client::instance().report(event(2));
    fixture.start();
    require(server.wait_requests(4), "failed batch retried after next heartbeat");
    fixture.stop();
    const auto requests = server.requests();
    require(requests[1].target == "/internal/runtime-events" && requests[3].target == "/internal/runtime-events", "batch retry follows heartbeat");
    require(requests[1].body == requests[3].body, "failed batch retained intact");
}

void test_new_events_follow_failed_batch()
{
    scripted_http_server server(
        {response_action::no_content, response_action::hold_server_error, response_action::no_content, response_action::no_content});
    client_fixture fixture(server.url());
    media_server::signaling_client::instance().report(event(1));
    media_server::signaling_client::instance().report(event(2));
    fixture.start();
    require(server.wait_requests(2), "old batch request held");
    media_server::signaling_client::instance().report(event(3));
    server.release_hold();
    require(server.wait_requests(4), "failed batch and new event retried");
    fixture.stop();
    const auto events = batch_events(server.requests()[3]);
    require(events.size() == 3U, "retry combines old and new events");
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        require(events[index].as_object().at("stream_id").as_string() == stream_id(index + 1U), "old events remain before new event");
    }
}

void test_capacity_and_overflow()
{
    scripted_http_server server;
    client_fixture fixture(server.url());
    for (std::size_t index = 0; index < 500U; ++index)
    {
        media_server::signaling_client::instance().report(event(index));
    }
    fixture.start();
    require(server.wait_requests(2), "capacity batch uploaded");
    fixture.stop();
    require(batch_events(server.requests()[1]).size() == 500U, "queue accepts exactly five hundred events");
}

void test_overflow_keeps_newest()
{
    scripted_http_server server;
    client_fixture fixture(server.url());
    for (std::size_t index = 0; index <= 500U; ++index)
    {
        media_server::signaling_client::instance().report(event(index));
    }
    fixture.start();
    require(server.wait_requests(2), "overflow batch uploaded");
    fixture.stop();
    const auto events = batch_events(server.requests()[1]);
    require(events.size() == 1U && events.front().as_object().at("stream_id").as_string() == stream_id(500),
            "overflow clears old backlog and keeps newest");
}

void test_report_does_not_run_network()
{
    scripted_http_server server;
    client_fixture fixture(server.url());
    const auto started = std::chrono::steady_clock::now();
    media_server::signaling_client::instance().report(event(1));
    require(std::chrono::steady_clock::now() - started < 100ms, "report does not block on network");
    require(!server.wait_requests(1, 100ms), "report does not initiate HTTP");
}

}    // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "signaling event test case required");
    const std::string_view test{argv[1]};
    if (test == "unconfigured")
    {
        test_unconfigured_report_is_noop();
    }
    else if (test == "heartbeat_failure")
    {
        test_heartbeat_failure_retains_events();
    }
    else if (test == "batch")
    {
        test_batch_order_and_identity();
    }
    else if (test == "retry")
    {
        test_failed_batch_is_retried();
    }
    else if (test == "retry_order")
    {
        test_new_events_follow_failed_batch();
    }
    else if (test == "capacity")
    {
        test_capacity_and_overflow();
    }
    else if (test == "overflow")
    {
        test_overflow_keeps_newest();
    }
    else if (test == "nonblocking")
    {
        test_report_does_not_run_network();
    }
    else
    {
        throw std::runtime_error("unknown signaling event test case");
    }
    return 0;
}
