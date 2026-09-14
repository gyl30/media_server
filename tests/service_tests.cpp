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
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "config.h"
#include "service.h"
#include "media/core/runtime_event.h"
#include "media/core/stream_registry.h"
#include "media/gb28181/gb28181_udp_sender_session.h"
#include "media/net/port_manager.h"
#include "media/net/worker_context.h"
#include "tests/clients/publish_claim_test_server.h"
#include "tests/clients/rtsp_test_client.h"

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

class track_barrier_sink final : public media_server::media_sink
{
   public:
    void on_track(const media_server::media_track&) override
    {
        {
            std::lock_guard lock(mutex_);
            track_received_ = true;
        }
        condition_.notify_all();
    }

    void on_frame(const media_server::media_frame&) override {}

    void on_end() override {}

    bool wait_for_track()
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this]() { return track_received_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool track_received_{};
};

void test_signaling_registration_precedes_media_listeners()
{
    controlled_registration_server signaling;
    auto cfg = signaling_config(signaling.url());
    const auto http_port = cfg.http_port;
    const std::array ports{cfg.rtmp_port, cfg.rtsp_port, cfg.http_port};
    media_server::worker_context source_worker;
    auto source = std::make_shared<media_server::media_stream>("live/service-flv", source_worker);
    require(source->set_tracks({media_server::media_track{
                .id = 1,
                .kind = media_server::media_kind::video,
                .codec = media_server::codec_id::h264,
                .clock_rate = 90'000,
                .channel_count = 0,
                .codec_config = {},
            }}), "service stop source track");
    require(media_server::stream_registry::instance().add(source), "service stop source registration");
    std::jthread source_runner([&]() { source_worker.run(); });
    media_server::service service(std::move(cfg));
    std::atomic<int> result{-1};
    std::jthread runner([&]() { result.store(service.run()); });

    const bool registration_started = signaling.wait_request();
    const bool listening_before_registration = can_connect(http_port);
    signaling.release();
    const bool listening_after_registration = wait_listening(http_port);
    boost::asio::io_context clients_io;
    std::vector<boost::asio::ip::tcp::socket> clients;
    for (const auto port : ports)
    {
        clients.emplace_back(clients_io);
        clients.back().connect({boost::asio::ip::make_address("127.0.0.1"), port});
    }
    boost::asio::ip::tcp::socket control(clients_io);
    control.connect({boost::asio::ip::make_address("127.0.0.1"), http_port});
    boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::post, "/gb28181/receiver/create", 11};
    request.set(boost::beast::http::field::content_type, "application/json");
    request.body() =
        R"({"stream_id":"550e8400-e29b-41d4-a716-446655440000","stream_name":"live/service-stop","transport":"udp","payload_type":96,"ssrc":305419896})";
    request.prepare_payload();
    boost::beast::http::write(control, request);
    boost::beast::flat_buffer response_buffer;
    boost::beast::http::response<boost::beast::http::string_body> response;
    boost::beast::http::read(control, response_buffer, response);
    require(response.result() == boost::beast::http::status::created, "service creates active udp receiver before stopping");
    const auto udp_port = static_cast<std::uint16_t>(boost::json::parse(response.body()).as_object().at("rtp_port").as_int64());
    clients.emplace_back(clients_io);
    auto& flv = clients.back();
    flv.connect({boost::asio::ip::make_address("127.0.0.1"), http_port});
    boost::beast::http::request<boost::beast::http::empty_body> flv_request{boost::beast::http::verb::get, "/live/service-flv.flv", 11};
    boost::beast::http::write(flv, flv_request);
    boost::beast::flat_buffer flv_buffer;
    boost::beast::http::response_parser<boost::beast::http::empty_body> flv_response;
    boost::beast::http::read_header(flv, flv_buffer, flv_response);
    require(flv_response.get().result() == boost::beast::http::status::ok, "service hands off active flv connection");
    if (result.load() == -1)
    {
        std::raise(SIGTERM);
    }
    runner.join();
    boost::asio::post(source_worker.io(),
                      [&]()
                      {
                          source->end();
                          source_worker.release_work();
                      });
    source_runner.join();

    require(registration_started, "service starts signaling registration");
    require(!listening_before_registration, "media listeners wait for signaling registration");
    require(listening_after_registration, "media listeners start after signaling registration");
    require(result.load() == 0, "service stops cleanly after signaling registration");
    for (const auto port : ports)
    {
        require(!can_connect(port), "service closes listeners before run returns");
    }
    for (auto& client : clients)
    {
        client.non_blocking(true);
        std::array<char, 1024> buffer{};
        boost::system::error_code error;
        while (!error)
        {
            client.read_some(boost::asio::buffer(buffer), error);
        }
        require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset,
                "service closes active connections before run returns");
    }
    const auto released_ports = media_server::port_manager::instance().acquire_pair();
    require(released_ports && released_ports->first == udp_port, "service releases active udp reservation before run returns");
    {
        boost::asio::ip::udp::socket rtp(clients_io, {boost::asio::ip::address_v4::loopback(), released_ports->first});
        boost::asio::ip::udp::socket rtcp(clients_io, {boost::asio::ip::address_v4::loopback(), released_ports->second});
    }
    media_server::port_manager::instance().release(*released_ports);
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

void test_service_shutdown_latches_output_reason()
{
    constexpr std::string_view publisher_stream_id = "00000000-0000-4000-8000-000000000001";
    constexpr std::string_view output_stream_id = "00000000-0000-4000-8000-000000000002";
    constexpr std::string_view stream_name = "live/service-shutdown-reason";
    constexpr std::string_view sender_id = "shutdown-reason";

    media_server::test::publish_claim_test_server signaling;
    auto cfg = signaling_config(signaling.url());
    cfg.threads = 1;
    const auto rtsp_port = cfg.rtsp_port;
    media_server::service service(std::move(cfg));
    std::atomic<int> result{-1};
    std::jthread runner([&]() { result.store(service.run()); });
    require(wait_listening(rtsp_port), "service shutdown reason RTSP listener starts");

    boost::asio::io_context publisher_io;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(rtsp_port) + '/' + std::string(stream_name);
    const auto sdp = std::string("v=0\r\n") +
                     "o=- 0 0 IN IP4 127.0.0.1\r\n"
                     "s=publish\r\n"
                     "c=IN IP4 127.0.0.1\r\n"
                     "t=0 0\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 H264/90000\r\n"
                     "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9oB4AiflwFuQA==,aM48gA==\r\n"
                     "a=control:" +
                     base + "/trackID=1\r\n";
    media_server::test::rtsp_test_client publisher(
        publisher_io, "/" + std::string(stream_name) + "?stream_id=" + std::string(publisher_stream_id));
    const std::vector<std::uint8_t> rtp{
        0x80, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x65, 0x88, 0x84, 0x21, 0xa0};
    auto publish = boost::asio::co_spawn(
        publisher_io, publisher.publish("127.0.0.1", rtsp_port, sdp, rtp), boost::asio::use_future);
    publisher_io.run();
    require(!publish.get(), "service shutdown reason publisher starts");

    auto stream = media_server::stream_registry::instance().find(stream_name);
    require(stream != nullptr, "service shutdown reason publisher enters registry");
    media_server::worker_context output_worker;
    std::vector<media_server::runtime_event> events;
    bool owner_only = true;
    const auto runtime_events = std::make_shared<media_server::runtime_event_emitter>(
        "media-1",
        "instance-1",
        [&output_worker, &events, &owner_only](media_server::runtime_event event)
        {
            owner_only = owner_only && output_worker.io().get_executor().running_in_this_thread();
            events.push_back(std::move(event));
        });
    const media_server::gb28181_transport_config transport{
        .mode = media_server::gb28181_transport::udp,
        .remote_address = boost::asio::ip::address_v4::loopback(),
        .remote_rtp_port = 9,
        .remote_rtcp_port = 10,
        .payload_type = 96,
        .ssrc = 0x12345678,
    };
    auto output = std::make_shared<media_server::gb28181_udp_sender_session>(
        output_worker,
        std::string(output_stream_id),
        stream,
        transport,
        boost::asio::ip::address_v4::loopback(),
        std::string(sender_id),
        false,
        std::chrono::seconds{25},
        1024U * 1024U,
        runtime_events);
    auto& streams = media_server::stream_registry::instance();
    require(streams.add_sender_session(std::string(stream_name), std::string(sender_id), output),
            "service shutdown reason output reservation");
    bool output_started = false;
    boost::asio::post(output_worker.io(), [&output, &output_started]() { output_started = output->startup(); });
    static_cast<void>(output_worker.io().run_one());
    require(output_started, "service shutdown reason output starts");
    auto reader_barrier = std::make_shared<track_barrier_sink>();
    stream->add_sink(reader_barrier);
    require(reader_barrier->wait_for_track(), "service shutdown reason output reader attaches");
    require(events.size() == 1U && events[0].type == media_server::runtime_event_type::output_started,
            "service shutdown reason output emits starting");

    std::raise(SIGTERM);
    runner.join();
    require(result.load() == 0, "service shutdown reason service stops");
    output_worker.release_work();
    output_worker.io().restart();
    output_worker.run();

    require(events.size() == 2U && events[1].type == media_server::runtime_event_type::output_stopped,
            "service shutdown reason output stops once");
    require(events[1].end_reason == media_server::runtime_end_reason::server_shutdown,
            "service shutdown reason is latched before source end");
    require(owner_only, "service shutdown reason events stay on output owner worker");
}

}    // namespace

int main()
{
    media_server::port_manager::init(media_server::default_media_port_start, media_server::default_media_port_end);
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
    test_service_shutdown_latches_output_reason();

    std::cout << "[pass] service tests\n";
    media_server::stream_registry::instance().clear();
    media_server::port_manager::destroy();
    return 0;
}
