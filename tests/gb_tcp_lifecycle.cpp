#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/core/media_stream.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_tcp_receiver_session.h"
#include "media/gb28181/gb28181_tcp_sender_session.h"
#include "media/net/worker_context.h"

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

void receiver_queued_read()
{
    worker_context worker;
    boost::asio::io_context peer_io;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor upstream(peer_io, {address, 0});
    tcp::socket peer(peer_io);
    std::shared_ptr<gb28181_tcp_receiver_session> receiver;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (receiver)
            {
                receiver->shutdown();
                receiver.reset();
            }
            boost::system::error_code error;
            peer.close(error);
            upstream.close(error);
            worker.request_stop();
            worker.io().restart();
            worker.io().run_for(3s);
        });
    bool accepted{};
    boost::system::error_code accept_error;
    upstream.async_accept(peer,
                          [&](boost::system::error_code error)
                          {
                              accept_error = error;
                              accepted = true;
                          });
    receiver = std::make_shared<gb28181_tcp_receiver_session>(worker, "verify/gb-tcp-queued-read", 96, 1);
    require(session_registry::instance().add_receiver_session("verify/gb-tcp-queued-read", receiver), "TCP receiver registration failed");
    require(receiver->startup(upstream.local_endpoint()), "TCP receiver startup failed");
    require(worker.io().poll() != 0, "TCP receiver connect was not initiated");
    peer_io.run_for(3s);
    require(accepted && !accept_error, "TCP peer did not accept receiver before deadline");
    worker.io().poll();

    const std::array<std::uint8_t, 15> frame{0, 13, 0x80, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    boost::asio::write(peer, boost::asio::buffer(frame));
    const std::weak_ptr<gb28181_tcp_receiver_session> lifetime = receiver;
    receiver->shutdown();
    receiver->shutdown();
    receiver.reset();
    worker.io().poll();
    require(!session_registry::instance().find_receiver_session("verify/gb-tcp-queued-read"), "TCP receiver shutdown retained registry ownership");
    peer.non_blocking(true);
    std::array<std::uint8_t, 1> response{};
    boost::system::error_code error;
    peer.read_some(boost::asio::buffer(response), error);
    require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset, "TCP receiver shutdown retained peer socket");
    worker.request_stop();
    worker.io().run_for(3s);
    require(worker.io().stopped() && lifetime.expired(), "TCP receiver late read retained session or asynchronous work");
    std::cout << "GB TCP receiver queued RTP read and repeated parent shutdown: PASS\n";
}

void sender_establishment_shutdown(bool refused)
{
    worker_context worker;
    boost::asio::io_context peer_io;
    const auto address = boost::asio::ip::address_v4::loopback();
    tcp::acceptor upstream(peer_io, {address, 0});
    tcp::socket reserved(peer_io);
    reserved.open(tcp::v4());
    reserved.bind({address, 0});
    tcp::socket peer(peer_io);
    auto source = std::make_shared<media_stream>("verify/gb-tcp-establishment", worker);
    require(source->set_tracks({{.id = 1, .kind = media_kind::video, .codec = codec_id::h264, .clock_rate = 90'000, .codec_config = {}}}),
            "TCP sender source tracks rejected");
    std::shared_ptr<gb28181_tcp_sender_session> sender;
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (sender)
            {
                sender->shutdown();
                sender.reset();
            }
            boost::system::error_code error;
            peer.close(error);
            upstream.close(error);
            reserved.close(error);
            worker.request_stop();
            worker.io().restart();
            worker.io().run_for(3s);
        });
    const std::string sender_id = refused ? "refused" : "connected";
    sender = std::make_shared<gb28181_tcp_sender_session>(worker, source, sender_id);
    require(session_registry::instance().add_sender_session(source->stream_id(), sender_id, sender), "TCP sender registration failed");
    sender->startup(refused ? reserved.local_endpoint() : upstream.local_endpoint(), 96, 1);
    require(worker.io().poll_one() == 1, "TCP sender connect was not initiated");
    if (!refused)
    {
        bool accepted{};
        boost::system::error_code accept_error;
        upstream.async_accept(peer,
                              [&](boost::system::error_code error)
                              {
                                  accept_error = error;
                                  accepted = true;
                              });
        peer_io.run_for(3s);
        require(accepted && !accept_error, "TCP peer did not accept sender before deadline");
    }
    const std::weak_ptr<gb28181_tcp_sender_session> lifetime = sender;
    sender->shutdown();
    sender->shutdown();
    sender.reset();
    worker.io().poll();
    require(!session_registry::instance().take_sender_session(source->stream_id(), sender_id), "TCP sender shutdown retained registry ownership");
    worker.request_stop();
    worker.io().run_for(3s);
    require(worker.io().stopped() && lifetime.expired(), "TCP sender establishment completion retained session or asynchronous work");
    std::cout << "GB TCP sender " << sender_id << " establishment and repeated parent shutdown: PASS\n";
}
}    // namespace

int main(int argc, char* argv[])
{
    try
    {
        require(argc == 2, "one GB TCP lifecycle scenario is required");
        const std::string_view scenario = argv[1];
        if (scenario == "receiver_queued_read")
        {
            receiver_queued_read();
        }
        else
        {
            require(scenario == "sender_connected" || scenario == "sender_refused", "unknown GB TCP lifecycle scenario");
            sender_establishment_shutdown(scenario == "sender_refused");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "GB TCP lifecycle: " << error.what() << '\n';
        return 1;
    }
}
