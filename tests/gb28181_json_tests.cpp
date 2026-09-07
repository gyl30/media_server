#include <string>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include "media/http/gb28181_json.h"

namespace media_server
{
namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string{message});
    }
}

void test_receiver_configs()
{
    const auto udp = parse_gb28181_receiver_config(
        R"({"stream_name":"live/camera","transport":"udp","payload_type":96,"ssrc":0})");
    require(udp && udp->stream_name == "live/camera" && udp->transport.mode == gb28181_transport::udp &&
                udp->transport.payload_type == 96 && udp->transport.ssrc == 0,
            "receiver udp");

    const auto tcp_active = parse_gb28181_receiver_config(
        R"({"stream_name":"live/tcp","transport":"tcp_active","remote_address":"192.168.1.10","remote_port":30000,"payload_type":96,"ssrc":100})");
    require(tcp_active && tcp_active->transport.mode == gb28181_transport::tcp_active &&
                tcp_active->transport.remote_address.to_string() == "192.168.1.10" &&
                tcp_active->transport.remote_port == 30000,
            "receiver tcp active");

    const auto tcp_passive = parse_gb28181_receiver_config(
        R"({"stream_name":"live/tcp","transport":"tcp_passive","listen_port":30000,"payload_type":96,"ssrc":100})");
    require(tcp_passive && tcp_passive->transport.mode == gb28181_transport::tcp_passive &&
                tcp_passive->transport.listen_port == 30000,
            "receiver tcp passive");

    const std::string invalid[] = {
        R"({"stream_name":"live/x","transport":"udp","payload_type":96})",
        R"({"stream_name":"live/x","transport":"udp","payload_type":96,"ssrc":1,"unknown":true})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","rtp_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","payload_type":128,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","payload_type":96,"ssrc":-1})",
        R"({"stream_name":"live/x","transport":"tcp_active","remote_address":"127.0.0.1","payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_active","remote_address":"0.0.0.0","remote_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_active","address":"127.0.0.1","rtp_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_passive","payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_passive","listen_port":31000,"remote_address":"127.0.0.1","payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"TCP","remote_address":"127.0.0.1","remote_port":31000,"payload_type":96,"ssrc":1})",
        "{",
        R"([])",
    };
    for (std::size_t index = 0; index < std::size(invalid); ++index)
    {
        require(!parse_gb28181_receiver_config(invalid[index]), "invalid receiver rejected at case " + std::to_string(index));
    }
}

void test_sender_configs()
{
    const auto udp = parse_gb28181_sender_config(
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100})");
    require(udp && udp->sender_id == "platform-a" && !udp->rtcp_enabled &&
                udp->transport.remote_rtp_port == 32000 && udp->transport.remote_rtcp_port == 0,
            "sender udp");

    const auto rtcp = parse_gb28181_sender_config(
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"remote_rtcp_port":32001,"payload_type":96,"ssrc":100,"rtcp_enabled":true})");
    require(rtcp && rtcp->rtcp_enabled && rtcp->transport.remote_rtcp_port == 32001, "sender udp rtcp");

    const auto tcp_active = parse_gb28181_sender_config(
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_active","remote_address":"192.168.1.20","remote_port":32000,"payload_type":96,"ssrc":100})");
    require(tcp_active && tcp_active->transport.mode == gb28181_transport::tcp_active &&
                tcp_active->transport.remote_port == 32000,
            "sender tcp active");

    const auto tcp_passive = parse_gb28181_sender_config(
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_passive","listen_port":32000,"payload_type":96,"ssrc":100})");
    require(tcp_passive && tcp_passive->transport.mode == gb28181_transport::tcp_passive &&
                tcp_passive->transport.listen_port == 32000,
            "sender tcp passive");

    const std::string invalid[] = {
        R"({"stream_name":"live/camera","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"0.0.0.0","remote_rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"remote_rtcp_port":32000,"payload_type":96,"ssrc":100,"rtcp_enabled":true})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100,"rtcp_enabled":true})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"remote_rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_active","remote_address":"127.0.0.1","remote_port":32000,"payload_type":96,"ssrc":100,"rtcp_enabled":true})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_active","remote_address":"127.0.0.1","remote_port":32000,"remote_rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_active","remote_address":"0.0.0.0","remote_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"tcp_passive","listen_port":32000,"remote_address":"127.0.0.1","payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","sender_id":"platform-a","transport":"udp","remote_address":"192.168.1.20","remote_rtp_port":32000,"payload_type":96,"ssrc":100,"unknown":1})",
    };
    for (std::size_t index = 0; index < std::size(invalid); ++index)
    {
        require(!parse_gb28181_sender_config(invalid[index]), "invalid sender rejected at case " + std::to_string(index));
    }
}

void test_delete_configs()
{
    const auto receiver = parse_gb28181_receiver_delete(R"({"stream_name":"live/camera"})");
    require(receiver && *receiver == "live/camera", "receiver delete");
    require(!parse_gb28181_receiver_delete(R"({"stream_name":""})"), "empty receiver delete");
    require(!parse_gb28181_receiver_delete(R"({"stream_name":"live/camera","extra":1})"), "receiver delete extra field");

    const auto sender = parse_gb28181_sender_delete(R"({"stream_name":"live/camera","sender_id":"platform-a"})");
    require(sender && sender->first == "live/camera" && sender->second == "platform-a", "sender delete");
    require(!parse_gb28181_sender_delete(R"({"stream_name":"live/camera"})"), "missing sender delete id");
    require(!parse_gb28181_sender_delete(R"({"stream_name":"live/camera","output_id":"platform-a"})"), "old sender delete id rejected");
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_receiver_configs();
        media_server::test_sender_configs();
        media_server::test_delete_configs();
        std::cout << "[pass] gb28181_json_configs\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] gb28181_json_configs: " << error.what() << '\n';
        return 1;
    }
}
