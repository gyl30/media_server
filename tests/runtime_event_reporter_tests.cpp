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
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/beast.hpp>
#include <boost/url/parse.hpp>

#include "media/net/port_manager.h"
#include "media/http/gb28181_http.h"
#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/http/runtime_event_reporter.h"

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
    accepted,
    bad_request,
    server_error,
    disconnect,
    hold_no_content,
};

struct captured_request
{
    std::string target;
    std::string body;
    std::chrono::steady_clock::time_point received_at;
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
        {
            std::lock_guard lock(mutex_);
            hold_released_ = true;
        }
        condition_.notify_all();
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

    [[nodiscard]] bool wait_responses(std::size_t count, std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count]() { return responses_completed_ >= count; });
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
                requests_.push_back({std::string(request.target()), request.body(), std::chrono::steady_clock::now()});
            }
            condition_.notify_all();

            if (action == response_action::hold_no_content)
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

            const auto status = [action]()
            {
                switch (action)
                {
                    case response_action::accepted:
                        return boost::beast::http::status::accepted;
                    case response_action::bad_request:
                        return boost::beast::http::status::bad_request;
                    case response_action::server_error:
                        return boost::beast::http::status::internal_server_error;
                    case response_action::no_content:
                    case response_action::hold_no_content:
                    case response_action::disconnect:
                        return boost::beast::http::status::no_content;
                }
                return boost::beast::http::status::internal_server_error;
            }();
            boost::beast::http::response<boost::beast::http::string_body> response{status, request.version()};
            if (action == response_action::accepted)
            {
                response.body() = "not-json";
            }
            response.prepare_payload();
            boost::beast::http::write(socket, response, error);
            {
                std::lock_guard lock(mutex_);
                ++responses_completed_;
            }
            condition_.notify_all();
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
    std::size_t responses_completed_{};
    bool hold_released_{};
    std::thread thread_;
};

std::string stream_id(std::size_t value)
{
    std::ostringstream output;
    output << "00000000-0000-4000-8000-" << std::setfill('0') << std::setw(12) << value;
    return output.str();
}

std::string json_string(const boost::json::value& value) { return std::string(value.as_string()); }

media_server::runtime_event event(std::size_t value)
{
    return {
        .kind = media_server::runtime_kind::source,
        .server_id = "media-1",
        .instance_id = "instance-a",
        .stream_id = stream_id(value),
        .stream_name = "live/camera-" + std::to_string(value),
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
        .heartbeat_interval = 1h,
        .request_timeout = 2s,
    };
}

class reporter_fixture
{
   public:
    explicit reporter_fixture(std::string url)
        : work_(boost::asio::make_work_guard(io_)),
          client_(std::make_shared<media_server::signaling_client>(io_, client_options(std::move(url)))),
          reporter_(std::make_shared<media_server::runtime_event_reporter>(io_, client_)),
          runner_([this]() { io_.run(); })
    {
    }

    ~reporter_fixture()
    {
        if (reporter_)
        {
            reporter_->shutdown();
        }
        work_.reset();
        if (runner_.joinable())
        {
            runner_.join();
        }
    }

    void report(media_server::runtime_event value) { reporter_->report(std::move(value)); }

    void barrier()
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool reached{};
        boost::asio::post(io_,
                          [&]()
                          {
                              {
                                  std::lock_guard lock(mutex);
                                  reached = true;
                              }
                              condition.notify_all();
                          });
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, 2s, [&]() { return reached; }), "reporter owner barrier");
    }

    void shutdown()
    {
        reporter_->shutdown();
        reporter_.reset();
        work_.reset();
    }

    [[nodiscard]] std::weak_ptr<media_server::runtime_event_reporter> weak_reporter() const { return reporter_; }
    [[nodiscard]] boost::asio::io_context& io() { return io_; }

   private:
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::shared_ptr<media_server::signaling_client> client_;
    std::shared_ptr<media_server::runtime_event_reporter> reporter_;
    std::jthread runner_;
};

void test_fifo_and_event_body()
{
    scripted_http_server server({response_action::accepted, response_action::no_content, response_action::no_content});
    reporter_fixture fixture(server.url());
    auto first = event(1);
    first.state = media_server::runtime_state::stopped;
    first.source_id = "10000000-0000-4000-8000-000000000001";
    first.end_reason = media_server::runtime_end_reason::runtime_error;
    first.error = "connection_failed";
    fixture.report(std::move(first));
    fixture.report(event(2));
    fixture.report(event(3));
    require(server.wait_requests(3), "runtime events delivered in FIFO order");

    const auto requests = server.requests();
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        require(requests[index].target == "/internal/runtime-events", "runtime event target");
        const auto body = boost::json::parse(requests[index].body).as_object();
        require(json_string(body.at("stream_id")) == stream_id(index + 1U), "runtime event FIFO stream id");
        require(body.at("server_id") == "media-1" && body.at("instance_id") == "instance-a", "runtime event server identity");
    }
    const auto body = boost::json::parse(requests.front().body).as_object();
    require(body.size() == 11U, "runtime event optional fields serialized");
    require(body.at("kind") == "source" && body.at("protocol") == "rtsp", "runtime event enum fields serialized");
    require(body.at("state") == "stopped" && body.at("stage") == "connecting", "runtime event state serialized");
    require(body.at("source_id") == "10000000-0000-4000-8000-000000000001", "runtime event source id serialized");
    require(body.at("end_reason") == "runtime_error" && body.at("error") == "connection_failed", "runtime event terminal fields serialized");
    const auto second = boost::json::parse(requests[1].body).as_object();
    require(second.size() == 8U && !second.contains("source_id") && !second.contains("end_reason") && !second.contains("error"),
            "runtime event absent optional fields omitted");
}

void test_http_failures_drop_attempted_and_continue()
{
    scripted_http_server server({response_action::bad_request, response_action::server_error, response_action::no_content});
    reporter_fixture fixture(server.url());
    const auto started = std::chrono::steady_clock::now();
    fixture.report(event(1));
    fixture.report(event(2));
    fixture.report(event(3));
    require(server.wait_requests(3), "HTTP failures do not stop runtime event delivery");
    require(std::chrono::steady_clock::now() - started < 1s, "HTTP failures continue without reconnect delay");
    const auto requests = server.requests();
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        require(json_string(boost::json::parse(requests[index].body).as_object().at("stream_id")) == stream_id(index + 1U),
                "HTTP failure drops attempted event before continuing");
    }
}

void test_network_failure_drops_attempted_and_delays_backlog()
{
    scripted_http_server server({response_action::disconnect, response_action::no_content});
    reporter_fixture fixture(server.url());
    fixture.report(event(1));
    fixture.report(event(2));
    require(server.wait_requests(1), "network failure attempted event");
    require(!server.wait_requests(2, 1s), "network failure retains backlog during reconnect wait");
    require(server.wait_requests(2, 6s), "network failure resumes backlog after reconnect wait");
    const auto requests = server.requests();
    require(json_string(boost::json::parse(requests[0].body).as_object().at("stream_id")) == stream_id(1),
            "network failure attempted event is first");
    require(json_string(boost::json::parse(requests[1].body).as_object().at("stream_id")) == stream_id(2),
            "network failure does not retry attempted event");
    const auto delay = requests[1].received_at - requests[0].received_at;
    require(delay >= 4500ms && delay < 7s, "network reconnect delay is five seconds");
}

void test_queue_accepts_exact_capacity()
{
    scripted_http_server server({response_action::hold_no_content});
    reporter_fixture fixture(server.url());
    fixture.report(event(0));
    require(server.wait_requests(1), "capacity test holds attempted event");
    for (std::size_t index = 1; index <= 500U; ++index)
    {
        fixture.report(event(index));
    }
    fixture.barrier();
    server.release_hold();
    require(server.wait_requests(501, 8s), "queue retains exactly five hundred pending events");
    const auto requests = server.requests();
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        require(json_string(boost::json::parse(requests[index].body).as_object().at("stream_id")) == stream_id(index),
                "capacity queue preserves every pending event in order");
    }
}

void test_queue_overflow_clears_backlog_and_keeps_newest()
{
    scripted_http_server server({response_action::hold_no_content});
    reporter_fixture fixture(server.url());
    fixture.report(event(0));
    require(server.wait_requests(1), "overflow test holds attempted event");
    for (std::size_t index = 1; index <= 501U; ++index)
    {
        fixture.report(event(index));
    }
    fixture.barrier();
    server.release_hold();
    require(server.wait_requests(2), "overflow retains newest event");
    require(!server.wait_requests(3, 1s), "overflow clears every old pending event");
    const auto requests = server.requests();
    require(requests.size() == 2U, "overflow clears old pending backlog");
    require(json_string(boost::json::parse(requests.back().body).as_object().at("stream_id")) == stream_id(501),
            "overflow keeps newest incoming event");
}

void test_report_coalesces_before_owner_context()
{
    scripted_http_server server({response_action::hold_no_content});
    boost::asio::io_context io;
    auto client = std::make_shared<media_server::signaling_client>(io, client_options(server.url()));
    auto reporter = std::make_shared<media_server::runtime_event_reporter>(io, client);
    for (std::size_t index = 0; index <= 500U; ++index)
    {
        reporter->report(event(index));
    }

    bool barrier_reached{};
    boost::asio::post(io, [&barrier_reached]() { barrier_reached = true; });
    std::size_t handlers{};
    while (!barrier_reached && handlers < 600U)
    {
        require(io.poll_one() == 1U, "reporter owner barrier remains runnable");
        ++handlers;
    }
    require(barrier_reached, "reporter owner barrier reached");
    require(handlers < 10U, "runtime event ingress coalesces owner handlers");

    reporter->shutdown();
    server.release_hold();
    io.run_for(500ms);
}

void test_emit_never_blocks_worker()
{
    scripted_http_server server({response_action::hold_no_content});
    reporter_fixture fixture(server.url());
    fixture.report(event(0));
    require(server.wait_requests(1), "nonblocking test holds delivery");

    boost::asio::io_context worker;
    bool completed{};
    const auto reporter = fixture.weak_reporter().lock();
    require(reporter != nullptr, "nonblocking reporter exists");
    const auto emitter = std::make_shared<media_server::runtime_event_emitter>(
        "media-1", "instance-a", [reporter](media_server::runtime_event value) { reporter->report(std::move(value)); });
    boost::asio::post(worker,
                      [&]()
                      {
                          emitter->emit(event(1));
                          completed = true;
                      });
    const auto started = std::chrono::steady_clock::now();
    worker.run();
    require(completed && std::chrono::steady_clock::now() - started < 100ms, "runtime event emission does not block worker");
    server.release_hold();
}

media_server::gb28181_http_request gb_receiver_request(std::string stream_name, std::string id)
{
    media_server::gb28181_http_request request{boost::beast::http::verb::post, "/gb28181/receiver/create", 11};
    request.set(boost::beast::http::field::content_type, "application/json");
    request.body() = boost::json::serialize(boost::json::object{
        {"stream_id", std::move(id)},
        {"stream_name", std::move(stream_name)},
        {"transport", "udp"},
        {"payload_type", 96},
        {"ssrc", 305419896},
    });
    request.prepare_payload();
    return request;
}

media_server::gb28181_http_response handle_gb_receiver(media_server::worker_context& worker,
                                                       media_server::gb28181_http_request request,
                                                       media_server::runtime_event_emitter_ptr runtime_events)
{
    const auto target = boost::urls::parse_origin_form(request.target());
    require(target.has_value(), "GB28181 test request target");
    return media_server::handle_gb28181_receiver_request(
        request, worker, *target, boost::asio::ip::address_v4::loopback(), std::move(runtime_events));
}

void test_delivery_failure_does_not_stop_media_session()
{
    scripted_http_server server({response_action::server_error});
    reporter_fixture fixture(server.url());
    media_server::worker_context worker;
    const auto reporter = fixture.weak_reporter();
    const auto emitter = std::make_shared<media_server::runtime_event_emitter>("media-1",
                                                                               "instance-a",
                                                                               [reporter](media_server::runtime_event value)
                                                                               {
                                                                                   if (const auto target = reporter.lock())
                                                                                   {
                                                                                       target->report(std::move(value));
                                                                                   }
                                                                               });
    constexpr std::string_view name = "live/event-delivery-failure";
    const auto id = stream_id(700);
    const auto create = handle_gb_receiver(worker, gb_receiver_request(std::string(name), id), emitter);
    require(create.result() == boost::beast::http::status::created, "GB28181 receiver starts before event failure");
    std::jthread runner([&]() { worker.run(); });
    require(server.wait_requests(1), "GB28181 starting event delivery attempted");
    require(server.wait_responses(1), "GB28181 starting event HTTP failure completed");
    fixture.report(event(701));
    require(server.wait_requests(2), "reporter continues after GB28181 event HTTP failure");

    const auto duplicate = handle_gb_receiver(worker, gb_receiver_request(std::string(name), id), emitter);
    require(duplicate.result() == boost::beast::http::status::internal_server_error, "event delivery failure leaves receiver session running");

    media_server::gb28181_http_request remove{boost::beast::http::verb::post, "/gb28181/receiver/delete", 11};
    remove.set(boost::beast::http::field::content_type, "application/json");
    remove.body() = boost::json::serialize(boost::json::object{{"stream_id", id}, {"stream_name", name}});
    remove.prepare_payload();
    const auto removed = handle_gb_receiver(worker, std::move(remove), emitter);
    require(removed.result() == boost::beast::http::status::no_content, "event delivery failure permits normal receiver shutdown");
    worker.release_work();
    runner.join();
    require(server.wait_requests(2), "GB28181 stopped event remains best-effort after prior HTTP failure");
    media_server::stream_registry::instance().clear();
}

void test_shutdown_cancels_reconnect_without_drain()
{
    scripted_http_server server({response_action::disconnect});
    reporter_fixture fixture(server.url());
    fixture.report(event(1));
    fixture.report(event(2));
    require(server.wait_requests(1), "shutdown test reaches network failure");
    require(!server.wait_requests(2, 1s), "shutdown test enters reconnect wait");
    const auto weak = fixture.weak_reporter();
    const auto started = std::chrono::steady_clock::now();
    fixture.shutdown();
    while (!weak.expired() && std::chrono::steady_clock::now() - started < 500ms)
    {
        std::this_thread::sleep_for(5ms);
    }
    require(weak.expired(), "reporter shutdown cancels reconnect timer");
    require(std::chrono::steady_clock::now() - started < 500ms, "reporter shutdown does not drain backlog");
    require(!server.wait_requests(2, 200ms), "reporter shutdown does not send queued event");
}

void test_shutdown_cancels_in_flight_without_drain()
{
    scripted_http_server server({response_action::hold_no_content});
    reporter_fixture fixture(server.url());
    fixture.report(event(1));
    fixture.report(event(2));
    require(server.wait_requests(1), "in-flight shutdown test holds HTTP response");
    const auto weak = fixture.weak_reporter();
    const auto started = std::chrono::steady_clock::now();
    fixture.shutdown();
    while (!weak.expired() && std::chrono::steady_clock::now() - started < 500ms)
    {
        std::this_thread::sleep_for(5ms);
    }
    require(weak.expired(), "reporter shutdown cancels in-flight HTTP request");
    require(std::chrono::steady_clock::now() - started < 500ms, "in-flight shutdown does not wait for response or drain backlog");
    server.release_hold();
    require(!server.wait_requests(2, 200ms), "in-flight shutdown does not send queued event");
}

}    // namespace

int main()
{
    media_server::port_manager::init(media_server::default_media_port_start, media_server::default_media_port_end);
    media_server::stream_registry::instance().clear();
    test_fifo_and_event_body();
    test_http_failures_drop_attempted_and_continue();
    test_network_failure_drops_attempted_and_delays_backlog();
    test_queue_accepts_exact_capacity();
    test_queue_overflow_clears_backlog_and_keeps_newest();
    test_report_coalesces_before_owner_context();
    test_emit_never_blocks_worker();
    test_delivery_failure_does_not_stop_media_session();
    test_shutdown_cancels_in_flight_without_drain();
    test_shutdown_cancels_reconnect_without_drain();
    media_server::stream_registry::instance().clear();
    media_server::port_manager::destroy();
    return 0;
}
