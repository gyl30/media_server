#include <iostream>
#include <iterator>
#include <string>

#include <boost/asio/io_context.hpp>

#include "clients/webrtc_client.h"

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 2;
    }
    boost::asio::io_context io;
    auto context = media_server::bench::webrtc_client_context::create();
    const auto direction = std::string(argv[1]) == "publish" ? media_server::bench::webrtc_client_direction::publish
                                                           : media_server::bench::webrtc_client_direction::play;
    std::cout << context->make_offer(direction) << "phase=offer_ready" << std::endl;
    const std::string answer{std::istreambuf_iterator<char>(std::cin), {}};
    auto peer = std::make_shared<media_server::bench::webrtc_client_peer>(io, context);
    std::string error;
    const bool established = peer->establish(answer, error);
    const auto local_port = peer->local_port();
    peer->close();
    const bool expected = std::string(argv[2]) == "established";
    std::cout << "established=" << established << " local_port=" << local_port << " error=" << error << '\n';
    return established == expected && (expected || error == "DTLS handshake timeout") ? 0 : 1;
}
