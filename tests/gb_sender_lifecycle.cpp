#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_rtp_sender.h"
#include "media/gb28181/gb28181_udp_sender_session.h"
#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/ps/mpeg_ps_output.h"

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

template <typename Function>
void on_owner(worker_context& worker, Function function)
{
    boost::asio::post(worker.io(), std::move(function));
    require(worker.io().poll() != 0, "owner operation did not run");
}

struct fixture
{
    worker_context source_worker;
    worker_context sender_worker;
    std::shared_ptr<media_stream> source;
    byte_buffer payload;

    fixture()
    {
        const std::vector<std::uint8_t> config{
            0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6e, 0x40, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
        };
        auto key = config;
        key.insert(key.end(), {0, 0, 0, 1, 0x65});
        key.resize(160, 0x55);
        payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(key));
        on_owner(source_worker,
                 [&]()
                 {
                     source = std::make_shared<media_stream>("verify/gb-sender-lifecycle", source_worker);
                     require(source->set_tracks(
                                 {{.id = 1, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90000, .codec_config = config}}),
                             "source tracks rejected");
                 });
    }

    ~fixture()
    {
        source_worker.request_stop();
        sender_worker.request_stop();
        source_worker.io().run_for(5s);
        sender_worker.io().run_for(5s);
    }

    void publish_and_end()
    {
        on_owner(source_worker,
                 [&]()
                 {
                     source->publish({.track = 1, .dts_ns = 1'000'000, .pts_ns = 1'000'000, .key_frame = true, .payload = payload});
                     source->end();
                 });
    }
};

struct observed_sink final : mpeg_ps_sink
{
    worker_context& owner;
    unsigned frames{};
    unsigned ends{};

    explicit observed_sink(worker_context& worker) : owner(worker) {}
    worker_context& worker() noexcept override { return owner; }
    void on_ps_frame(const mpeg_ps_frame&) override { ++frames; }
    void on_end() override { ++ends; }
};

void late_output_after_shutdown()
{
    fixture live;
    unsigned packets = 0;
    unsigned ends = 0;
    auto sender =
        std::make_shared<gb28181_rtp_sender>(live.sender_worker, live.source, [&](std::vector<std::uint8_t>) { ++packets; }, [&]() { ++ends; });
    on_owner(live.sender_worker,
             [&]()
             {
                 require(sender->startup(96, 1), "RTP sender startup failed");
                 sender->shutdown();
             });

    std::weak_ptr<mpeg_ps_output> output;
    on_owner(live.source_worker, [&]() { output = live.source->ps_output(); });
    live.sender_worker.io().poll();
    live.source_worker.io().poll();
    require(output.expired(), "late source result retained PS output after sender shutdown");
    live.publish_and_end();
    live.sender_worker.io().poll();
    require(packets == 0 && ends == 0, "closed sender received late output callbacks");
    const std::weak_ptr<gb28181_rtp_sender> lifetime = sender;
    sender.reset();
    require(lifetime.expired(), "late output tasks retained RTP sender");
    std::cout << "GB sender late source result after shutdown: PASS\n";
}

void queued_frames_after_shutdown()
{
    fixture live;
    unsigned packets = 0;
    unsigned ends = 0;
    auto sender =
        std::make_shared<gb28181_rtp_sender>(live.sender_worker, live.source, [&](std::vector<std::uint8_t>) { ++packets; }, [&]() { ++ends; });
    on_owner(live.sender_worker, [&]() { require(sender->startup(96, 2), "RTP sender startup failed"); });
    live.source_worker.io().poll();
    live.sender_worker.io().poll();
    auto observer = std::make_shared<observed_sink>(live.sender_worker);
    std::shared_ptr<mpeg_ps_output> output;
    on_owner(live.source_worker,
             [&]()
             {
                 output = live.source->ps_output();
                 output->add_sink(observer);
             });

    sender->shutdown();
    sender->shutdown();
    live.publish_and_end();
    require(live.sender_worker.io().run_one_for(5s) == 1, "first sender cleanup did not run");
    require(observer->frames == 0 && observer->ends == 0, "PS drain ran before sender cleanup");
    live.sender_worker.io().poll();
    require(observer->frames == 1 && observer->ends == 1, "queued PS frame/end did not actually drain");
    require(packets == 0 && ends == 0, "queued callback used a closed RTP sender");
    const std::weak_ptr<gb28181_rtp_sender> lifetime = sender;
    sender.reset();
    require(lifetime.expired(), "PS dispatcher retained a closed RTP sender");
    std::cout << "GB sender queued PS/end and repeated shutdown: PASS\n";
}

void parent_shutdown_before_sender_drain()
{
    fixture live;
    const auto address = boost::asio::ip::address_v4::loopback();
    udp::socket rtp_client(live.sender_worker.io(), {address, 0});
    udp::socket rtcp_client(live.sender_worker.io(), {address, 0});
    auto session = std::make_shared<gb28181_udp_sender_session>(
        live.sender_worker, live.source, "sender", rtp_client.local_endpoint(), rtcp_client.local_endpoint());
    on_owner(live.sender_worker,
             [&]()
             {
                 require(session_registry::instance().add_sender_session(live.source->stream_id(), "sender", session),
                         "UDP sender registration failed");
                 require(session->startup(address, 96, 3), "UDP sender startup failed");
             });
    live.source_worker.io().poll();
    live.sender_worker.io().poll();
    auto observer = std::make_shared<observed_sink>(live.sender_worker);
    on_owner(live.source_worker, [&]() { live.source->ps_output()->add_sink(observer); });
    session->shutdown();
    live.publish_and_end();

    require(live.sender_worker.io().run_one_for(5s) == 1, "parent shutdown did not run before drain");
    require(observer->frames == 0 && observer->ends == 0, "PS drain ran before parent cleanup");
    const auto returned = media_port_pool::instance().acquire();
    require(returned == 24'440 && !media_port_pool::instance().acquire(), "parent shutdown did not return its port pair");
    require(live.sender_worker.io().run_one_for(5s) == 1, "PS drain did not run before child shutdown");
    require(observer->frames == 1 && observer->ends == 1, "parent/child shutdown window did not include PS/end callbacks");

    const std::weak_ptr<gb28181_udp_sender_session> lifetime = session;
    session.reset();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline)
    {
        live.sender_worker.io().run_one_for(20ms);
    }
    require(lifetime.expired(), "pending child cleanup retained UDP sender session");
    require(!session_registry::instance().take_sender_session(live.source->stream_id(), "sender"), "shutdown retained registry ownership");

    std::array<std::uint8_t, 2048> buffer{};
    rtp_client.non_blocking(true);
    boost::system::error_code error;
    udp::endpoint peer;
    rtp_client.receive_from(boost::asio::buffer(buffer), peer, 0, error);
    require(error == boost::asio::error::would_block || error == boost::asio::error::try_again, "RTP was transmitted after parent resources closed");
    udp::socket released_rtp(live.sender_worker.io(), {address, *returned});
    udp::socket released_rtcp(live.sender_worker.io(), {address, static_cast<std::uint16_t>(*returned + 1U)});
    media_port_pool::instance().release(*returned);
    std::cout << "GB UDP parent cleanup before PS drain and child cleanup: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        media_port_pool::init(24'440, 24'441);
        late_output_after_shutdown();
        queued_frames_after_shutdown();
        parent_shutdown_before_sender_drain();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "GB sender lifecycle: " << error.what() << '\n';
        return 1;
    }
}
