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

void test_input_configs()
{
    const auto udp = parse_gb28181_input_config(
        R"({"stream_name":"live/camera","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":0})");
    require(udp.has_value(), "valid input udp");
    require(udp->stream_name == "live/camera" && udp->description.transport == gb28181_transport::udp && udp->description.rtp_port == 0 &&
                udp->description.rtcp_port == 0 && udp->description.payload_type == 96 && udp->description.ssrc == 0,
            "input udp values");

    const auto tcp_active = parse_gb28181_input_config(
        R"({"stream_name":"live/tcp","transport":"tcp_active","address":"192.168.1.10","rtp_port":30000,"payload_type":96,"ssrc":100})");
    require(tcp_active && tcp_active->description.transport == gb28181_transport::tcp_active && tcp_active->description.rtcp_port == 0,
            "input tcp active");

    const auto tcp_passive = parse_gb28181_input_config(
        R"({"stream_name":"live/tcp","transport":"tcp_passive","address":"127.0.0.1","rtp_port":30000,"payload_type":96,"ssrc":100})");
    require(tcp_passive && tcp_passive->description.transport == gb28181_transport::tcp_passive, "input tcp passive");

    const std::string invalid[] = {
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":1,"unknown":true})",
        R"({"stream_name":"live/x","transport":"udp","address":"bad","payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":128,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":-1})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","rtp_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","rtcp_port":31001,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":1,"remote_rtp_address":"127.0.0.1"})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":1,"remote_rtp_port":30000})",
        R"({"stream_name":"live/x","transport":"udp","address":"127.0.0.1","payload_type":96,"ssrc":1,"remote_rtcp_port":30001})",
        R"({"stream_name":"live/x","transport":"tcp_active","address":"127.0.0.1","rtp_port":31000,"rtcp_port":31001,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_active","address":"0.0.0.0","rtp_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_passive","address":"0.0.0.0","rtp_port":31000,"payload_type":96,"ssrc":1})",
        R"({"stream_name":"live/x","transport":"tcp_passive","address":"0.0.0.0","rtp_port":31000,"payload_type":96,"ssrc":1,"remote_rtcp_port":30001})",
        R"({"stream_name":"live/x","transport":"TCP","address":"127.0.0.1","rtp_port":31000,"payload_type":96,"ssrc":1})",
        "{",
        R"([])",
    };
    for (std::size_t index = 0; index < std::size(invalid); ++index)
    {
        require(!parse_gb28181_input_config(invalid[index]), "invalid input rejected at case " + std::to_string(index));
    }
}

void test_output_configs()
{
    const auto udp = parse_gb28181_output_config(
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100})");
    require(udp && udp->output_id == "platform-a" && !udp->rtcp && udp->description.rtcp_port == 32001, "output udp");

    const auto rtcp = parse_gb28181_output_config(
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100,"rtcp":true})");
    require(rtcp && rtcp->rtcp, "output udp rtcp");

    const auto tcp = parse_gb28181_output_config(
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"tcp_passive","address":"127.0.0.1","rtp_port":32000,"payload_type":96,"ssrc":100})");
    require(tcp && tcp->description.transport == gb28181_transport::tcp_passive, "output tcp");

    const std::string invalid[] = {
        R"({"stream_name":"live/camera","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"0.0.0.0","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"tcp_active","address":"127.0.0.1","rtp_port":32000,"payload_type":96,"ssrc":100,"rtcp":true})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"tcp_active","address":"127.0.0.1","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"tcp_active","address":"0.0.0.0","rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"tcp_passive","address":"0.0.0.0","rtp_port":32000,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100,"rtcp":1})",
        R"({"stream_name":"live/camera","output_id":"","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100})",
        R"({"stream_name":"live/camera","output_id":"platform-a","transport":"udp","address":"192.168.1.20","rtp_port":32000,"rtcp_port":32001,"payload_type":96,"ssrc":100,"unknown":1})",
    };
    for (std::size_t index = 0; index < std::size(invalid); ++index)
    {
        require(!parse_gb28181_output_config(invalid[index]), "invalid output rejected at case " + std::to_string(index));
    }
}

void test_delete_configs()
{
    const auto input = parse_gb28181_input_delete(R"({"stream_name":"live/camera"})");
    require(input && *input == "live/camera", "input delete");
    require(!parse_gb28181_input_delete(R"({"stream_name":""})"), "empty input delete");
    require(!parse_gb28181_input_delete(R"({"stream_name":"live/camera","extra":1})"), "input delete extra field");

    const auto output = parse_gb28181_output_delete(R"({"stream_name":"live/camera","output_id":"platform-a"})");
    require(output && output->first == "live/camera" && output->second == "platform-a", "output delete");
    require(!parse_gb28181_output_delete(R"({"stream_name":"live/camera"})"), "missing output delete id");
    require(!parse_gb28181_output_delete(R"({"stream_name":"live/camera","output_id":"platform-a","rtcp":false})"), "output delete extra field");
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_input_configs();
        media_server::test_output_configs();
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
