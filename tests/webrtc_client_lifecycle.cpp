#include <span>
#include <memory>
#include <cstdint>
#include <iostream>

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>

#include "bench/clients/webrtc_client.h"

int main()
{
    using media_server::bench::webrtc_client_context;
    using media_server::bench::webrtc_client_peer;
    struct parent
    {
        std::shared_ptr<webrtc_client_peer> peer;
    };

    auto context = webrtc_client_context::create();
    if (!context)
    {
        std::cerr << "WebRTC client context creation failed\n";
        return 1;
    }
    const std::weak_ptr<webrtc_client_context> weak_context = context;
    for (int iteration = 0; iteration < 51; ++iteration)
    {
        boost::asio::io_context io;
        auto peer = std::make_shared<webrtc_client_peer>(io, context, boost::asio::ip::address_v4::loopback());
        const auto port = peer->local_port();
        auto owner = std::make_shared<parent>(parent{peer});
        const std::weak_ptr<parent> weak_owner = owner;
        const std::weak_ptr<webrtc_client_peer> weak_peer = peer;
        int callbacks = 0;
        peer->start_receive(
            [owner, &callbacks](bool, std::span<const std::uint8_t>)
            {
                ++callbacks;
                owner->peer->close();
            },
            [owner, &callbacks](const boost::system::error_code&)
            {
                ++callbacks;
                owner->peer->close();
                owner->peer->close();
            });
        const bool send_error = iteration == 50;
        if (!send_error)
        {
            peer->close();
            peer->close();
        }
        // 最后一轮让既有 keepalive 定时器向未配置的端点发送并触发错误回调。
        owner.reset();
        peer.reset();
        io.run();
        if (!weak_owner.expired() || !weak_peer.expired() || callbacks != (send_error ? 1 : 0))
        {
            std::cerr << "WebRTC client callback ownership did not release, iteration=" << iteration << '\n';
            return 1;
        }
        boost::asio::ip::udp::socket replacement(io, {boost::asio::ip::address_v4::loopback(), port});
    }
    context.reset();
    if (!weak_context.expired())
    {
        std::cerr << "WebRTC client context retained after peer cleanup\n";
        return 1;
    }
    std::cout << "WebRTC client strong callbacks, repeated close, late completion, send error and port release: PASS\n";
}
