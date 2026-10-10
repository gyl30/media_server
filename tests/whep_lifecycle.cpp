#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/core/media_stream.h"
#include "media/net/media_port_pool.h"
#include "media/net/udp_transport.h"
#include "media/net/worker_context.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/webrtc_sdp.h"
#include "media/webrtc/whep_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using udp = boost::asio::ip::udp;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct observing_sink final : media_sink
{
    worker_context& owner;
    std::weak_ptr<whep_session> session;
    bool close_on_frame{};
    unsigned frames{};
    unsigned ends{};

    explicit observing_sink(worker_context& worker) : owner(worker) {}
    worker_context& worker() noexcept override { return owner; }
    void on_frame(const media_frame&) override
    {
        require(owner.io().get_executor().running_in_this_thread(), "cross-worker frame bypassed sink executor");
        ++frames;
        if (close_on_frame)
        {
            if (const auto current = session.lock())
            {
                current->shutdown();
                current->shutdown();
            }
            close_on_frame = false;
        }
    }
    void on_end() override
    {
        require(owner.io().get_executor().running_in_this_thread(), "cross-worker end bypassed sink executor");
        ++ends;
    }
};

void lifecycle(const std::string& scenario)
{
    worker_context source_worker;
    worker_context viewer_worker;
    const auto address = boost::asio::ip::address_v4::loopback();
    auto source = std::make_shared<media_stream>("whep-lifecycle", source_worker);
    require(source->set_tracks(
                {{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}}),
            "PCMA tracks rejected");
    const auto port = media_port_pool::instance().acquire();
    require(port.has_value(), "test media port unavailable");
    boost::scope::scope_exit release_initial_port([port]() { media_port_pool::instance().release(*port); });
    std::shared_ptr<whep_session> session;
    auto transport = std::make_shared<udp_transport>(viewer_worker.io());
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (session)
            {
                session->shutdown();
                session.reset();
            }
            boost::asio::post(source_worker.io(), [source]() { source->end(); });
            source_worker.request_stop();
            viewer_worker.request_stop();
            source_worker.io().restart();
            viewer_worker.io().restart();
            source_worker.io().run_for(1s);
            viewer_worker.io().run_for(1s);
        });
    boost::system::error_code error;
    transport->startup(address, *port, error);
    require(!error, "WHEP test UDP bind failed");
    udp::socket peer(viewer_worker.io(), {address, 0});
    const auto certificate = dtls_certificate::create();
    require(certificate != nullptr, "WHEP test certificate failed");
    const auto offer = parse_webrtc_offer(
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=recvonly\r\n"
        "a=rtcp-mux\r\na=setup:actpass\r\na=ice-ufrag:remote-test\r\na=ice-pwd:remote-test-password\r\n"
        "a=fingerprint:sha-256 " +
        certificate->sha256_fingerprint() +
        "\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=rtpmap:8 PCMA/8000\r\n");
    require(offer.has_value(), "WHEP test offer invalid");
    const auto* transport_offer = find_webrtc_transport(*offer);
    require(transport_offer != nullptr, "WHEP test transport offer missing");
    webrtc_answer_config config{.address = address,
                                .port = *port,
                                .stream_id = scenario,
                                .ice_ufrag = "local-test",
                                .ice_pwd = "local-test-password",
                                .fingerprint = certificate->sha256_fingerprint()};
    const auto answer = make_webrtc_answer(*offer, source->tracks(), config);
    require(answer.has_value(), "WHEP test answer invalid");
    auto observer = std::make_shared<observing_sink>(viewer_worker);
    source->add_sink(observer);
    source_worker.io().poll();
    session = std::make_shared<whep_session>(viewer_worker, source, transport, std::move(config));
    release_initial_port.set_active(false);
    observer->session = session;
    observer->close_on_frame = scenario == "snapshot_shutdown";
    bool started{};
    boost::asio::post(viewer_worker.io(), [&]() { started = session->startup(*transport_offer, *answer, *certificate); });
    viewer_worker.io().poll();
    require(started, "WHEP test session startup failed");
    source_worker.io().poll();
    const std::weak_ptr<whep_session> lifetime = session;
    const std::weak_ptr<udp_transport> transport_lifetime = transport;
    const media_frame frame{.track = 1,
                            .dts_ns = 20'000'000,
                            .pts_ns = 20'000'000,
                            .key_frame = false,
                            .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5)};
    boost::asio::post(viewer_worker.io(),
                      [transport, endpoint = peer.local_endpoint()]()
                      {
                          require(transport->write(std::vector<std::uint8_t>(256, 1), endpoint), "first WHEP transport write rejected");
                          require(transport->write(std::vector<std::uint8_t>(256, 2), endpoint), "second WHEP transport write rejected");
                      });
    viewer_worker.io().poll_one();
    const std::array<std::uint8_t, 20> incoming{0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42};
    peer.send_to(boost::asio::buffer(incoming), {address, *port});
    if (scenario == "repeated_shutdown")
    {
        std::thread caller(
            [current = session]()
            {
                current->shutdown();
                current->shutdown();
                current->shutdown();
            });
        caller.join();
    }
    else
    {
        if (scenario == "queued_shutdown")
        {
            session->shutdown();
        }
        boost::asio::post(source_worker.io(),
                          [source, frame]()
                          {
                              source->publish(frame);
                              source->publish(frame);
                              source->end();
                          });
        source_worker.io().poll();
        require(observer->frames == 0 && observer->ends == 0, "cross-worker callback ran before target drain");
    }
    viewer_worker.io().poll();
    source_worker.io().poll();
    boost::asio::post(viewer_worker.io(),
                      [transport, current = session, frame, endpoint = peer.local_endpoint()]()
                      {
                          boost::system::error_code endpoint_error;
                          static_cast<void>(transport->local_endpoint(endpoint_error));
                          require(endpoint_error == boost::asio::error::bad_descriptor, "WHEP shutdown retained UDP socket");
                          require(!transport->write(std::vector<std::uint8_t>{3}, endpoint), "WHEP write after cleanup accepted");
                          current->on_frame(frame);
                          current->on_end();
                          current->shutdown();
                          current->shutdown();
                      });
    const auto reacquired = media_port_pool::instance().acquire();
    require(reacquired == port, "WHEP cleanup failed to return its media port");
    boost::scope::scope_exit release_reacquired([&]() { media_port_pool::instance().release(*reacquired); });
    udp::socket rebound(viewer_worker.io(), {address, *reacquired});
    viewer_worker.io().poll();
    require(!media_port_pool::instance().acquire(), "repeated WHEP cleanup released another owner's allocation");
    if (scenario != "repeated_shutdown")
    {
        require(observer->frames == 2 && observer->ends == 1, "cross-worker frame/end sequence changed");
    }
    session.reset();
    transport.reset();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!lifetime.expired() || !transport_lifetime.expired()) && std::chrono::steady_clock::now() < deadline)
    {
        source_worker.io().poll();
        viewer_worker.io().run_one_for(20ms);
    }
    require(lifetime.expired() && transport_lifetime.expired(), "WHEP cleanup retained session or transport ownership");
    source_worker.request_stop();
    viewer_worker.request_stop();
    source_worker.io().run_for(1s);
    viewer_worker.io().run_for(1s);
    require(source_worker.io().stopped() && viewer_worker.io().stopped(), "WHEP cleanup retained worker tasks or timers");
    std::cout << "WHEP " << scenario << ": PASS\n";
}
}    // namespace

int main()
{
    try
    {
        media_port_pool::init(24'800, 24'801);
        for (const auto* scenario : {"repeated_shutdown", "queued_shutdown", "snapshot_shutdown"})
        {
            lifecycle(scenario);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WHEP lifecycle: " << error.what() << '\n';
        return 1;
    }
}
