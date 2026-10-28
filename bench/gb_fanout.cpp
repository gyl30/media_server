#include <string>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include "bench/media_fanout.h"
#include "bench/clients/gb_client.h"

namespace
{

constexpr std::uint16_t default_control_port = 18080;

std::string option_value(int& index, int argc, char** argv)
{
    if (index + 1 >= argc)
    {
        throw std::runtime_error("missing value for " + std::string(argv[index]));
    }
    return argv[++index];
}

media_server::bench::gb_transport parse_transport(std::string_view text)
{
    if (text == "udp")
    {
        return media_server::bench::gb_transport::udp;
    }
    if (text == "tcp_active")
    {
        return media_server::bench::gb_transport::tcp_active;
    }
    if (text == "tcp_passive")
    {
        return media_server::bench::gb_transport::tcp_passive;
    }
    throw std::runtime_error("transport must be udp, tcp_active or tcp_passive");
}

media_server::bench::gb_peer parse_peer(std::string_view text)
{
    if (text == "quiet")
    {
        return media_server::bench::gb_peer::quiet;
    }
    if (text == "rtcp")
    {
        return media_server::bench::gb_peer::rtcp;
    }
    if (text == "rtcp_rr")
    {
        return media_server::bench::gb_peer::rtcp_rr;
    }
    if (text == "rr")
    {
        return media_server::bench::gb_peer::rr;
    }
    if (text == "rtp_noise")
    {
        return media_server::bench::gb_peer::rtp_noise;
    }
    throw std::runtime_error("peer must be quiet, rtcp, rtcp_rr, rr or rtp_noise");
}

std::uint64_t percentile(std::vector<std::uint64_t> values, std::size_t percent)
{
    if (values.empty())
    {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1U) * percent / 100U];
}

}    // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "--control-token-file FILE --transport udp|tcp_active|tcp_passive\n"
                     "--bind-address ADDRESS --listen-port PORT (TCP sessions use consecutive ports)\n"
                     "--peer quiet|rtcp|rtcp_rr|rr|rtp_noise (UDP: configure remote RTCP, send RR to RTP source port + 1 or junk to the RTP "
                     "source port each second)\n"
                     "--play-paths-file FILE (one stream_id/sender_id per viewer)\n";
        return media_server::bench::media_fanout<media_server::bench::gb_client>::run(argc, argv, default_control_port);
    }
    std::vector<char*> forwarded{argv[0]};
    std::string host;
    std::uint16_t port = default_control_port;
    try
    {
        media_server::bench::gb_client_options options;
        std::string token_file;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument = argv[index];
            if (argument == "--control-token-file")
            {
                token_file = option_value(index, argc, argv);
            }
            else if (argument == "--transport")
            {
                options.transport = parse_transport(option_value(index, argc, argv));
            }
            else if (argument == "--bind-address")
            {
                options.bind_address = boost::asio::ip::make_address(option_value(index, argc, argv));
            }
            else if (argument == "--peer")
            {
                options.peer = parse_peer(option_value(index, argc, argv));
            }
            else if (argument == "--listen-port")
            {
                options.listen_port = static_cast<std::uint16_t>(std::stoul(option_value(index, argc, argv)));
            }
            else
            {
                if (argument == "--media-host" && index + 1 < argc)
                {
                    host = argv[index + 1];
                }
                if (argument == "--media-port" && index + 1 < argc)
                {
                    port = static_cast<std::uint16_t>(std::stoul(argv[index + 1]));
                }
                forwarded.push_back(argv[index]);
            }
        }
        std::ifstream token(token_file);
        if (!std::getline(token, options.control_token) || options.control_token.empty() || options.bind_address.is_unspecified() || host.empty() ||
            (options.transport != media_server::bench::gb_transport::udp && options.listen_port == 0))
        {
            throw std::runtime_error("control-token-file, bind-address, media-host and a TCP listen-port are required");
        }
        media_server::bench::gb_client::configure(std::move(options));
    }
    catch (const std::exception& error)
    {
        std::cerr << "GB fanout configuration failed: " << error.what() << '\n';
        return 2;
    }

    const auto code =
        media_server::bench::media_fanout<media_server::bench::gb_client>::run(static_cast<int>(forwarded.size()), forwarded.data(), port);
    boost::asio::io_context io;
    std::size_t deleted{};
    const auto& senders = media_server::bench::gb_client::created_senders();
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void>
        {
            for (const auto& sender : senders)
            {
                if (!co_await media_server::bench::gb_client::delete_sender(host, port, sender))
                {
                    break;
                }
                ++deleted;
            }
        },
        boost::asio::detached);
    io.run();
    const auto& peer = media_server::bench::gb_client::peer_counters();
    std::cout << "phase=peer sender_reports=" << peer.sender_reports << " invalid_rtcp=" << peer.invalid_rtcp
              << " receiver_reports=" << peer.receiver_reports << " noise_datagrams=" << peer.noise_datagrams << std::endl;
    std::cout << "phase=cleanup created=" << senders.size() << " deleted=" << deleted << std::endl;
    const auto& creates = media_server::bench::gb_client::create_milliseconds();
    const auto& deletes = media_server::bench::gb_client::delete_milliseconds();
    std::cout << "phase=control create_requests=" << creates.size() << " create_ms_p50=" << percentile(creates, 50)
              << " create_ms_p90=" << percentile(creates, 90) << " create_ms_p99=" << percentile(creates, 99)
              << " create_ms_max=" << percentile(creates, 100) << " delete_requests=" << deletes.size()
              << " delete_ms_p50=" << percentile(deletes, 50) << " delete_ms_p90=" << percentile(deletes, 90)
              << " delete_ms_p99=" << percentile(deletes, 99) << " delete_ms_max=" << percentile(deletes, 100) << std::endl;
    return code == 0 && deleted == senders.size() ? 0 : 1;
}
