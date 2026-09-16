#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

struct captured_request
{
    std::string target;
    std::string body;
};

class test_http_server
{
   public:
    explicit test_http_server(boost::beast::http::status status = boost::beast::http::status::no_content,
                              std::string response_body = {},
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
            boost::beast::http::response<boost::beast::http::string_body> response{static_cast<boost::beast::http::status>(status_.load()),
                                                                                   request.version()};
            if (response.result_int() >= 200 && response.result_int() < 300)
            {
                response.body() = response_body_;
            }
            else
            {
                response.set(boost::beast::http::field::content_type, "application/json");
                response.body() = R"({"error":"rejected"})";
            }
            response.prepare_payload();
            boost::beast::http::write(socket, response, error);
        }
    }

   private:
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
        .rtmp_port = 1935,
        .rtsp_port = 8554,
        .http_port = 8080,
        .heartbeat_interval = 20ms,
        .request_timeout = 500ms,
    };
}

void configure_client(boost::asio::io_context& io, media_server::signaling_client_options options)
{
    media_server::signaling_client::instance().configure(io, std::move(options));
}

std::uint16_t unused_port()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

template <typename Operation>
media_server::signaling_request_result run_request(boost::asio::io_context& io, Operation&& operation)
{
    std::optional<media_server::signaling_request_result> result;
    boost::asio::spawn(io, [&](boost::asio::yield_context yield) { result = operation(yield); }, boost::asio::detached);
    io.run();
    io.restart();
    require(result.has_value(), "signaling request completed");
    return std::move(*result);
}

void test_unconfigured_accepts_without_network()
{
    boost::asio::io_context io;

    const auto registration =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    const auto heartbeat =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().heartbeat_once(yield); });
    const auto claim = run_request(
        io,
        [&](boost::asio::yield_context& yield)
        { return media_server::signaling_client::instance().claim_publish("00000000-0000-4000-8000-000000000001", "rtmp", "live/camera", yield); });
    const auto reported = run_request(io,
                                      [&](boost::asio::yield_context& yield)
                                      {
                                          const media_server::runtime_event event{};
                                          return media_server::signaling_client::instance().report_runtime_event(event, yield);
                                      });
    require(registration.kind == media_server::signaling_result_kind::accepted, "unconfigured registration accepted");
    require(heartbeat.kind == media_server::signaling_result_kind::accepted, "unconfigured heartbeat accepted");
    require(claim.kind == media_server::signaling_result_kind::accepted, "unconfigured publish claim accepted");
    require(reported.kind == media_server::signaling_result_kind::accepted, "unconfigured runtime event accepted");

    bool fenced{};
    bool completed{};
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            media_server::signaling_client::instance().run_heartbeat(yield, [&]() { fenced = true; });
            completed = true;
        },
        boost::asio::detached);
    io.run();
    require(completed && !fenced, "unconfigured heartbeat returns without fencing");
}

void test_registration_and_heartbeat_body()
{
    test_http_server server;
    boost::asio::io_context io;
    configure_client(io, client_options(server.url()));
    const auto registration_result =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(registration_result.kind == media_server::signaling_result_kind::accepted, "registration accepted");
    const auto heartbeat_result =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().heartbeat_once(yield); });
    require(heartbeat_result.kind == media_server::signaling_result_kind::accepted, "heartbeat accepted");
    const auto requests = server.wait_requests(2);
    require(requests[0].target == "/internal/media-servers/register", "registration target");
    require(requests[1].target == "/internal/media-servers/heartbeat", "heartbeat target");
    const auto registration = boost::json::parse(requests[0].body).as_object();
    const auto heartbeat = boost::json::parse(requests[1].body).as_object();
    require(registration.at("server_id") == "media-1", "registration server id");
    require(registration.at("instance_id") == "instance-a", "registration instance id");
    require(registration.at("control_url") == "http://127.0.0.1:8080", "registration control url");
    require(registration.at("media_ip") == "127.0.0.1", "registration media ip");
    require(registration.at("rtmp_port") == 1935, "registration RTMP port");
    require(registration.at("rtsp_port") == 8554, "registration RTSP port");
    require(registration.at("http_port") == 8080, "registration HTTP port");
    require(heartbeat.at("server_id") == "media-1" && heartbeat.at("instance_id") == "instance-a", "heartbeat identity stable");
}

void test_publish_claim_uses_caller_executor_and_body()
{
    test_http_server server;
    boost::asio::io_context construction_io;
    boost::asio::io_context worker_io;
    configure_client(construction_io, client_options(server.url()));
    const auto result = run_request(
        worker_io,
        [&](boost::asio::yield_context& yield)
        { return media_server::signaling_client::instance().claim_publish("00000000-0000-4000-8000-000000000001", "rtmp", "live/camera", yield); });
    require(result.kind == media_server::signaling_result_kind::accepted, "publish claim accepted on caller executor");
    const auto requests = server.wait_requests(1);
    require(requests[0].target == "/internal/publish/claim", "publish claim target");
    const auto body = boost::json::parse(requests[0].body).as_object();
    require(body.size() == 5U, "publish claim field count");
    require(body.at("stream_id") == "00000000-0000-4000-8000-000000000001", "publish claim stream id");
    require(body.at("server_id") == "media-1" && body.at("instance_id") == "instance-a", "publish claim server identity");
    require(body.at("protocol") == "rtmp", "publish claim protocol");
    require(body.at("stream_name") == "live/camera", "publish claim stream name");
}

media_server::runtime_event runtime_event()
{
    return {
        .kind = media_server::runtime_kind::source,
        .server_id = "media-1",
        .instance_id = "instance-a",
        .stream_id = "00000000-0000-4000-8000-000000000001",
        .stream_name = "live/camera",
        .source_id = "10000000-0000-4000-8000-000000000001",
        .protocol = media_server::runtime_protocol::rtsp,
        .state = media_server::runtime_state::stopped,
        .stage = "connecting",
        .end_reason = media_server::runtime_end_reason::runtime_error,
        .error = "connection_failed",
    };
}

void test_runtime_event_no_content()
{
    const auto event = runtime_event();
    test_http_server no_content(boost::beast::http::status::no_content, "");
    boost::asio::io_context no_content_io;
    configure_client(no_content_io, client_options(no_content.url()));
    const auto no_content_result =
        run_request(no_content_io,
                    [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().report_runtime_event(event, yield); });
    require(no_content_result.kind == media_server::signaling_result_kind::accepted && no_content_result.status == 204,
            "runtime event accepts empty 204 response");
    const auto request = no_content.wait_requests(1).front();
    require(request.target == "/internal/runtime-events", "runtime event endpoint");
    const auto body = boost::json::parse(request.body).as_object();
    require(body.size() == 11U && body.at("kind") == "source" && std::string(body.at("stream_id").as_string()) == event.stream_id,
            "runtime event body");
    require(std::string(body.at("source_id").as_string()) == *event.source_id && std::string(body.at("stage").as_string()) == *event.stage,
            "runtime event optional identity and stage");
    require(body.at("end_reason") == "runtime_error" && std::string(body.at("error").as_string()) == *event.error, "runtime event terminal fields");
}

void test_runtime_event_accepted()
{
    const auto event = runtime_event();
    test_http_server accepted(boost::beast::http::status::accepted, "not-json");
    boost::asio::io_context accepted_io;
    configure_client(accepted_io, client_options(accepted.url()));
    const auto accepted_result =
        run_request(accepted_io,
                    [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().report_runtime_event(event, yield); });
    require(accepted_result.kind == media_server::signaling_result_kind::accepted && accepted_result.status == 202,
            "runtime event accepts non-JSON 202 response");
}

void test_rejected_result()
{
    test_http_server server(boost::beast::http::status::conflict);
    boost::asio::io_context rejected_io;
    configure_client(rejected_io, client_options(server.url()));
    const auto rejected_result =
        run_request(rejected_io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(rejected_result.kind == media_server::signaling_result_kind::rejected && rejected_result.status == 409, "registration rejected");
}

void test_temporary_failure_result()
{
    test_http_server temporary(boost::beast::http::status::internal_server_error);
    boost::asio::io_context temporary_io;
    configure_client(temporary_io, client_options(temporary.url()));
    const auto temporary_result =
        run_request(temporary_io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(temporary_result.kind == media_server::signaling_result_kind::temporary_failure && temporary_result.status == 500,
            "server failure is temporary");
}

void test_network_error_result()
{
    auto options = client_options("http://127.0.0.1:" + std::to_string(unused_port()));
    boost::asio::io_context unavailable_io;
    configure_client(unavailable_io, std::move(options));
    const auto registration = run_request(
        unavailable_io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(registration.kind == media_server::signaling_result_kind::network_error, "registration network error");
    const auto heartbeat = run_request(
        unavailable_io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().heartbeat_once(yield); });
    require(heartbeat.kind == media_server::signaling_result_kind::network_error, "heartbeat network error");
}

void test_success_uses_status_only()
{
    test_http_server server(boost::beast::http::status::ok, "{invalid");
    boost::asio::io_context io;
    configure_client(io, client_options(server.url()));
    const auto result =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(result.kind == media_server::signaling_result_kind::accepted && result.status == 200, "success body ignored");
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
            boost::asio::io_context io;
            media_server::signaling_client::instance().configure(io, client_options(url));
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        require(rejected, "non-base signaling URL rejected");
    }
}

void test_request_timeout()
{
    test_http_server server(boost::beast::http::status::no_content, "", 2s);
    auto options = client_options(server.url());
    options.request_timeout = 20ms;
    boost::asio::io_context io;
    configure_client(io, std::move(options));
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        run_request(io, [&](boost::asio::yield_context& yield) { return media_server::signaling_client::instance().register_once(yield); });
    require(result.kind == media_server::signaling_result_kind::network_error && result.error == "request timeout", "request timeout classified");
    require(std::chrono::steady_clock::now() - started < 500ms, "request timeout cancels in-flight HTTP");
}

void test_completed_request_releases_cancellation_handler()
{
    auto options = client_options("http://127.0.0.1:" + std::to_string(unused_port()));
    boost::asio::io_context io;
    configure_client(io, std::move(options));
    boost::asio::cancellation_signal cancellation;
    bool completed = false;
    bool handler_released = false;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            yield.throw_if_cancelled(false);
            const auto result = media_server::signaling_client::instance().register_once(yield);
            completed = result.kind == media_server::signaling_result_kind::network_error;
            handler_released = !yield.get_cancellation_slot().has_handler();
        },
        boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::detached));
    io.run();
    require(completed, "signaling network failure completed");
    require(handler_released, "completed signaling request releases cancellation handler");
}

void test_heartbeat_rejection()
{
    test_http_server server(boost::beast::http::status::internal_server_error);
    boost::asio::io_context io;
    configure_client(io, client_options(server.url()));
    std::mutex mutex;
    std::condition_variable condition;
    bool fenced = false;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            media_server::signaling_client::instance().run_heartbeat(yield,
                                                                     [&]()
                                                                     {
                                                                         std::lock_guard lock(mutex);
                                                                         fenced = true;
                                                                         condition.notify_all();
                                                                     });
        },
        boost::asio::detached);
    std::jthread runner([&]() { io.run(); });
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
    runner.join();
}

void test_control_cancellation_ends_heartbeat_wait()
{
    test_http_server server;
    auto options = client_options(server.url());
    options.heartbeat_interval = 1h;
    boost::asio::io_context io;
    configure_client(io, std::move(options));
    boost::asio::cancellation_signal cancellation;
    bool completed = false;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            yield.throw_if_cancelled(false);
            media_server::signaling_client::instance().run_heartbeat(yield, []() {});
            completed = true;
        },
        boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::detached));
    std::jthread runner([&]() { io.run(); });
    const auto started = std::chrono::steady_clock::now();
    boost::asio::post(io, [&]() { cancellation.emit(boost::asio::cancellation_type::all); });
    runner.join();
    require(completed, "heartbeat coroutine completes after timer cancellation");
    require(std::chrono::steady_clock::now() - started < 500ms, "control cancellation ends heartbeat wait");
}

void test_control_cancellation_ends_in_flight_heartbeat()
{
    test_http_server server(boost::beast::http::status::no_content, "", 2s);
    auto options = client_options(server.url());
    options.heartbeat_interval = 1ms;
    options.request_timeout = 5s;
    boost::asio::io_context io;
    configure_client(io, std::move(options));
    boost::asio::cancellation_signal cancellation;
    bool completed = false;
    boost::asio::spawn(
        io,
        [&](boost::asio::yield_context yield)
        {
            yield.throw_if_cancelled(false);
            media_server::signaling_client::instance().run_heartbeat(yield, []() {});
            completed = true;
        },
        boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::detached));
    std::jthread runner([&]() { io.run(); });
    server.wait_requests(1);
    const auto started = std::chrono::steady_clock::now();
    boost::asio::post(io, [&]() { cancellation.emit(boost::asio::cancellation_type::all); });
    runner.join();
    require(completed, "heartbeat coroutine completes after request cancellation");
    require(std::chrono::steady_clock::now() - started < 500ms, "control cancellation ends in-flight heartbeat");
}

}    // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "signaling client test case required");
    const std::string_view test{argv[1]};
    if (test == "unconfigured")
    {
        test_unconfigured_accepts_without_network();
    }
    else if (test == "registration")
    {
        test_registration_and_heartbeat_body();
    }
    else if (test == "claim")
    {
        test_publish_claim_uses_caller_executor_and_body();
    }
    else if (test == "event_no_content")
    {
        test_runtime_event_no_content();
    }
    else if (test == "event_accepted")
    {
        test_runtime_event_accepted();
    }
    else if (test == "rejected")
    {
        test_rejected_result();
    }
    else if (test == "temporary_failure")
    {
        test_temporary_failure_result();
    }
    else if (test == "network_error")
    {
        test_network_error_result();
    }
    else if (test == "status_only")
    {
        test_success_uses_status_only();
    }
    else if (test == "invalid_url")
    {
        test_constructor_rejects_non_base_urls();
    }
    else if (test == "timeout")
    {
        test_request_timeout();
    }
    else if (test == "cancellation_release")
    {
        test_completed_request_releases_cancellation_handler();
    }
    else if (test == "heartbeat_rejection")
    {
        test_heartbeat_rejection();
    }
    else if (test == "heartbeat_wait_cancellation")
    {
        test_control_cancellation_ends_heartbeat_wait();
    }
    else if (test == "heartbeat_request_cancellation")
    {
        test_control_cancellation_ends_in_flight_heartbeat();
    }
    else
    {
        throw std::runtime_error("unknown signaling client test case");
    }
    return 0;
}
