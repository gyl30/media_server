#include <mutex>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/beast.hpp>

#include "media/net/port_manager.h"
#include "media/core/media_stream.h"
#include "media/core/runtime_event.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/gb28181/gb28181_types.h"
#include "media/gb28181/gb28181_udp_sender_session.h"

extern "C"
{
#include "rtp-packet.h"
}

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
        .kind = media_server::event_kind::source,
        .stream_id = stream_id(value),
        .stream_name = "live/camera-" + std::to_string(value),
        .source_id = "10000000-0000-4000-8000-000000000001",
        .protocol = media_server::event_protocol::rtsp,
        .state = media_server::event_state::starting,
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
            [](boost::asio::yield_context yield) { media_server::signaling_client::instance().run(yield); },
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

   private:
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::jthread runner_;
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
    first.state = media_server::event_state::runtime_error;
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
    require(terminal.at("state") == "runtime_error" && terminal.at("error") == "connection_failed", "fact event fields serialized");
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

void test_retry_overflow_keeps_new_events()
{
    scripted_http_server server(
        {response_action::no_content, response_action::hold_server_error, response_action::no_content, response_action::no_content});
    client_fixture fixture(server.url());
    for (std::size_t index = 0; index < 400U; ++index)
    {
        media_server::signaling_client::instance().report(event(index));
    }
    fixture.start();
    require(server.wait_requests(2), "old batch request held");
    for (std::size_t index = 400U; index < 600U; ++index)
    {
        media_server::signaling_client::instance().report(event(index));
    }
    server.release_hold();
    require(server.wait_requests(4), "new events retried after old backlog overflow");
    fixture.stop();
    const auto events = batch_events(server.requests()[3]);
    require(events.size() == 200U, "retry overflow drops old backlog and keeps newer events");
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        require(events[index].as_object().at("stream_id").as_string() == stream_id(index + 400U), "new events keep their order");
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

void test_gb_sender_streaming_is_not_repeated_after_config_update()
{
    scripted_http_server server;
    client_fixture fixture(server.url());
    fixture.start();

    media_server::port_manager::init(33'500, 33'599);
    media_server::worker_context worker;
    auto source = std::make_shared<media_server::media_stream>("live/gb-event-source", worker);
    boost::asio::io_context receiver_io;
    boost::asio::ip::udp::socket rtp_receiver(receiver_io, {boost::asio::ip::address_v4::loopback(), 0});
    boost::asio::ip::udp::socket rtcp_receiver(receiver_io, {boost::asio::ip::address_v4::loopback(), 0});

    constexpr media_server::track_id video_track_id = 1;
    constexpr std::string_view event_stream_id = "550e8400-e29b-41d4-a716-446655440000";
    const std::vector<std::uint8_t> initial_config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f,
        0x97, 0x01, 0x6e, 0x40, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    };
    const std::vector<std::uint8_t> updated_config{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xba, 0x6a, 0x02, 0x1a, 0x02, 0x80, 0x00,
        0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8c, 0x18, 0xcb, 0x00, 0x00, 0x00, 0x01, 0x68, 0xef, 0xbc, 0xb0,
    };
    const auto frame = [](std::int64_t timestamp, bool key_frame, const std::vector<std::uint8_t>& config)
    {
        std::vector<std::uint8_t> payload;
        if (key_frame)
        {
            payload = config;
            payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0});
        }
        else
        {
            payload = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
        }
        return media_server::media_frame{
            .track = video_track_id,
            .dts_ns = timestamp,
            .pts_ns = timestamp,
            .key_frame = key_frame,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)),
        };
    };

    const media_server::gb28181_transport_config config{
        .mode = media_server::gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = rtp_receiver.local_endpoint().port(),
        .remote_rtcp_port = rtcp_receiver.local_endpoint().port(),
        .payload_type = 96,
        .ssrc = 0x12345678U,
    };
    auto session = std::make_shared<media_server::gb28181_udp_sender_session>(
        worker, std::string{event_stream_id}, source, config, boost::asio::ip::address_v4::loopback(), "event-sender", false);
    std::promise<bool> started;
    auto started_result = started.get_future();
    boost::asio::post(worker.io(),
                      [source, session, &started, &initial_config, frame]()
                      {
                          const bool tracks = source->set_tracks({media_server::media_track{
                              .id = video_track_id,
                              .kind = media_server::media_kind::video,
                              .codec = media_server::codec_id::h264,
                              .clock_rate = 90'000,
                              .channel_count = 0,
                              .codec_config = initial_config,
                          }});
                          const bool registered =
                              media_server::stream_registry::instance().add(source) &&
                              media_server::stream_registry::instance().add_sender_session(source->name(), "event-sender", session);
                          const bool running = tracks && registered && session->startup();
                          started.set_value(running);
                          if (running)
                          {
                              source->publish(frame(0, true, initial_config));
                          }
                      });
    std::jthread runner([&worker]() { worker.run(); });
    const auto stop = [&worker, &runner, &fixture]()
    {
        worker.stop();
        if (runner.joinable())
        {
            runner.join();
        }
        fixture.stop();
    };
    if (!started_result.get())
    {
        stop();
        require(false, "gb sender event session starts");
    }

    const auto count_streaming = [&server, event_stream_id]()
    {
        std::size_t count{};
        for (const auto& request : server.requests())
        {
            if (request.target != "/internal/runtime-events")
            {
                continue;
            }
            for (const auto& value : batch_events(request))
            {
                const auto& object = value.as_object();
                if (object.at("stream_id").as_string() == event_stream_id && object.at("state") == "streaming")
                {
                    ++count;
                }
            }
        }
        return count;
    };

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (count_streaming() == 0U && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    if (count_streaming() != 1U)
    {
        stop();
        require(false, "gb sender first media reports streaming once");
    }
    std::vector<std::uint8_t> packet(65'536U);
    boost::asio::ip::udp::endpoint sender_endpoint;
    while (rtp_receiver.available() != 0U)
    {
        boost::system::error_code error;
        static_cast<void>(rtp_receiver.receive_from(boost::asio::buffer(packet), sender_endpoint, 0, error));
        if (error)
        {
            stop();
            require(false, "gb sender initial RTP drain");
        }
    }

    std::promise<bool> updated;
    auto updated_result = updated.get_future();
    boost::asio::post(worker.io(),
                      [source, &updated, &updated_config, frame]()
                      {
                          const bool changed = source->update_track(media_server::media_track{
                              .id = video_track_id,
                              .kind = media_server::media_kind::video,
                              .codec = media_server::codec_id::h264,
                              .clock_rate = 90'000,
                              .channel_count = 0,
                              .codec_config = updated_config,
                          });
                          source->publish(frame(40'000'000, false, updated_config));
                          source->publish(frame(80'000'000, true, updated_config));
                          updated.set_value(changed);
                      });
    if (!updated_result.get())
    {
        stop();
        require(false, "gb sender source config changes");
    }

    std::vector<std::uint8_t> updated_payload;
    const auto contains_updated_config = [&updated_payload, &updated_config]()
    { return std::search(updated_payload.begin(), updated_payload.end(), updated_config.begin(), updated_config.end()) != updated_payload.end(); };
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!contains_updated_config() && std::chrono::steady_clock::now() < deadline)
    {
        if (rtp_receiver.available() == 0U)
        {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        boost::system::error_code error;
        const auto bytes = rtp_receiver.receive_from(boost::asio::buffer(packet), sender_endpoint, 0, error);
        if (error)
        {
            stop();
            require(false, "gb sender updated RTP receive");
        }
        rtp_packet_t decoded{};
        if (rtp_packet_deserialize(&decoded, packet.data(), static_cast<int>(bytes)) != 0 || decoded.payloadlen == 0)
        {
            stop();
            require(false, "gb sender updated RTP parse");
        }
        const auto* begin = static_cast<const std::uint8_t*>(decoded.payload);
        updated_payload.insert(updated_payload.end(), begin, begin + decoded.payloadlen);
    }
    if (!contains_updated_config())
    {
        stop();
        require(false, "gb sender resumes media with updated config");
    }

    const auto marker_stream_id = stream_id(999);
    media_server::signaling_client::instance().report(event(999));
    const auto marker_delivered = [&server, &marker_stream_id]()
    {
        for (const auto& request : server.requests())
        {
            if (request.target != "/internal/runtime-events")
            {
                continue;
            }
            for (const auto& value : batch_events(request))
            {
                if (value.as_object().at("stream_id").as_string() == marker_stream_id)
                {
                    return true;
                }
            }
        }
        return false;
    };
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!marker_delivered() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    if (!marker_delivered())
    {
        stop();
        require(false, "gb sender event marker delivered");
    }
    if (count_streaming() != 1U)
    {
        stop();
        require(false, "gb sender config update does not repeat streaming");
    }

    std::promise<void> stopped;
    auto stopped_result = stopped.get_future();
    boost::asio::post(worker.io(),
                      [source, session, &stopped, &worker]()
                      {
                          session->shutdown();
                          media_server::stream_registry::instance().remove(*source);
                          source->end();
                          boost::asio::post(worker.io(), [&stopped]() { stopped.set_value(); });
                      });
    stopped_result.get();
    stop();
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
    else if (test == "retry_overflow")
    {
        test_retry_overflow_keeps_new_events();
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
    else if (test == "gb_sender_streaming")
    {
        test_gb_sender_streaming_is_not_repeated_after_config_update();
    }
    else
    {
        throw std::runtime_error("unknown signaling event test case");
    }
    return 0;
}
