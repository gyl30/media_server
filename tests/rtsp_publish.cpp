#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/scope/scope_exit.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/core/stream_registry.h"
#include "media/net/media_port_pool.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_publish_session.h"
#include "media/rtsp/rtsp_sdp.h"
#include "media/rtsp/rtsp_uri.h"

extern "C"
{
#include "rtsp-server.h"
}

namespace
{
using namespace media_server;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct publish_context
{
    worker_context& worker;
    std::string stream_id;
    std::string request_uri;
    std::shared_ptr<rtsp_publish_session> publisher;
    std::vector<std::string> replies;
    std::exception_ptr error;
    unsigned announce_calls{};
};

void content_urls(const std::string& scenario)
{
    const bool udp = scenario.starts_with("udp_");
    worker_context worker;
    publish_context context{.worker = worker, .stream_id = std::string(64, 'a'), .request_uri = {}, .publisher = {}, .replies = {}, .error = {}};
    context.request_uri = "rtsp://127.0.0.1:8554/" + context.stream_id;
    std::vector<std::unique_ptr<boost::asio::ip::udp::socket>> rtp_clients;
    std::vector<std::unique_ptr<boost::asio::ip::udp::socket>> rtcp_clients;
    rtsp_handler_t handler{};
    handler.close = [](void*) { return 0; };
    handler.send = [](void* param, const void* data, std::size_t bytes)
    {
        static_cast<publish_context*>(param)->replies.emplace_back(static_cast<const char*>(data), bytes);
        return 0;
    };
    handler.onannounce = [](void* param, rtsp_server_t* server, const char* uri, const char* sdp, int bytes)
    {
        auto& current = *static_cast<publish_context*>(param);
        try
        {
            ++current.announce_calls;
            require(uri != nullptr && uri == current.request_uri, "ANNOUNCE request URI changed");
            const auto target = parse_rtsp_target(uri);
            require(target && target->stream_id == current.stream_id, "ANNOUNCE target identity changed");
            current.publisher = std::make_shared<rtsp_publish_session>(
                current.worker, target->stream_id, boost::asio::ip::address_v4::loopback(), [](std::span<const std::uint8_t>) {}, []() {});
            if (!current.publisher->on_announce(server, uri, sdp, bytes))
            {
                return -1;
            }
            require(current.publisher->stream_id() == current.stream_id, "media control URI replaced source identity");
            return rtsp_server_reply_announce(server, 200);
        }
        catch (...)
        {
            current.error = std::current_exception();
            return -1;
        }
    };
    handler.onsetup =
        [](void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
    {
        auto& current = *static_cast<publish_context*>(param);
        try
        {
            require(current.publisher != nullptr, "SETUP reached an unannounced publisher");
            return current.publisher->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
        }
        catch (...)
        {
            current.error = std::current_exception();
            return -1;
        }
    };
    handler.onrecord = [](void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
    {
        auto& current = *static_cast<publish_context*>(param);
        try
        {
            require(current.publisher != nullptr, "RECORD reached an unannounced publisher");
            return current.publisher->on_record(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
        }
        catch (...)
        {
            current.error = std::current_exception();
            return -1;
        }
    };
    handler.onteardown = [](void* param, rtsp_server_t* server, const char* uri, const char* session)
    {
        auto& current = *static_cast<publish_context*>(param);
        try
        {
            require(current.publisher != nullptr, "TEARDOWN reached an unannounced publisher");
            return current.publisher->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
        }
        catch (...)
        {
            current.error = std::current_exception();
            return -1;
        }
    };
    std::unique_ptr<rtsp_server_t, decltype(&rtsp_server_destroy)> server(rtsp_server_create("127.0.0.1", 8554, &handler, &context, &context),
                                                                          &rtsp_server_destroy);
    require(server != nullptr, "RTSP server creation failed");
    boost::scope::scope_exit cleanup(
        [&]()
        {
            if (context.publisher)
            {
                context.publisher->shutdown();
            }
            worker.io().poll();
        });
    const auto input = [&](const std::string& request)
    {
        context.replies.clear();
        auto remaining = request.size();
        const auto result = rtsp_server_input(server.get(), request.data(), &remaining);
        if (context.error)
        {
            std::rethrow_exception(context.error);
        }
        require(remaining == 0, "RTSP input did not consume the complete request");
        return result;
    };

    const bool two_tracks = scenario == "content_base" || scenario == "absolute" || scenario == "udp_two" || scenario == "udp_incomplete";
    const std::string video_control = scenario == "absolute" ? "rtsp://127.0.0.1:8554/absolute/video" : "trackID=1";
    const std::string audio_control = scenario == "absolute" ? "rtsp://127.0.0.1:8554/absolute/audio" : "trackID=2";
    std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=publish-control\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM4G4g==\r\n"
        "a=control:" +
        video_control + "\r\n";
    if (two_tracks)
    {
        sdp +=
            "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/48000/2\r\n"
            "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1190;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n"
            "a=control:" +
            audio_control + "\r\n";
    }
    if (scenario == "invalid_sdp")
    {
        sdp = "invalid SDP";
    }
    std::string headers;
    std::string control_base = context.request_uri + "/";
    if (scenario == "content_base" || scenario == "absolute")
    {
        headers =
            "Content-Base: rtsp://127.0.0.1:8554/selected/base/\r\n"
            "Content-Location: rtsp://127.0.0.1:8554/ignored/location/\r\n";
        control_base = "rtsp://127.0.0.1:8554/selected/base/";
    }
    else if (scenario == "content_location")
    {
        headers = "Content-Location: rtsp://127.0.0.1:8554/selected/location/\r\n";
        control_base = "rtsp://127.0.0.1:8554/selected/location/";
    }
    const auto announce = "ANNOUNCE " + context.request_uri + " RTSP/1.0\r\nCSeq: 1\r\n" + headers +
                          "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    const auto result = input(announce);
    require(context.announce_calls == 1, "ANNOUNCE callback was not reached exactly once");
    if (scenario == "invalid_sdp")
    {
        require(result < 0 && context.replies.empty(), "invalid ANNOUNCE received a successful reply");
    }
    else
    {
        require(result == 0 && context.replies.size() == 1 && context.replies.front().starts_with("RTSP/1.0 200"), "valid ANNOUNCE failed");
        require(!stream_registry::instance().find(context.stream_id), "ANNOUNCE registered source before RECORD");
        std::string session_id;
        std::string first_setup;
        for (unsigned index = 0; index < (two_tracks && scenario != "udp_incomplete" ? 2U : 1U); ++index)
        {
            const auto uri =
                scenario == "absolute" ? (index == 0 ? video_control : audio_control) : control_base + (index == 0 ? video_control : audio_control);
            std::string transport;
            if (udp)
            {
                const auto endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0);
                for (auto* clients : {&rtp_clients, &rtcp_clients})
                {
                    auto client = std::make_unique<boost::asio::ip::udp::socket>(worker.io(), endpoint);
                    const auto port = client->local_endpoint().port();
                    require(port < 24'400 || port > 24'407, "client ephemeral port overlaps server media allocations");
                    clients->push_back(std::move(client));
                }
                transport = "RTP/AVP;unicast;client_port=" + std::to_string(rtp_clients.back()->local_endpoint().port()) + "-" +
                            std::to_string(rtcp_clients.back()->local_endpoint().port()) + ";mode=record";
            }
            else
            {
                transport = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(index * 2U) + "-" + std::to_string(index * 2U + 1U) + ";mode=record";
            }
            const auto setup = "SETUP " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(index + 2U) + "\r\n" +
                               (session_id.empty() ? "" : "Session: " + session_id + "\r\n") + "Transport: " + transport + "\r\n\r\n";
            require(input(setup) == 0 && context.replies.size() == 1 && context.replies.front().starts_with("RTSP/1.0 200"),
                    "resolved control URI SETUP failed");
            const auto& reply = context.replies.front();
            const auto begin = reply.find("Session: ");
            require(begin != std::string::npos, "SETUP reply omitted session identity");
            const auto end = reply.find_first_of(";\r\n", begin + 9U);
            require(end != std::string::npos, "SETUP session header was malformed");
            const auto returned_session_id = reply.substr(begin + 9U, end - begin - 9U);
            require(!returned_session_id.empty() && (session_id.empty() || session_id == returned_session_id), "SETUP changed session identity");
            session_id = returned_session_id;
            require(context.publisher->stream_id() == context.stream_id, "SETUP control URI replaced source identity");
            require(!stream_registry::instance().find(context.stream_id), "SETUP registered source before RECORD");
            if (index == 0)
            {
                first_setup = "SETUP " + uri + " RTSP/1.0\r\nCSeq: 9\r\nSession: " + session_id + "\r\nTransport: " + transport + "\r\n\r\n";
            }
        }
        if (udp)
        {
            if (scenario == "udp_duplicate")
            {
                require(input(first_setup) < 0 && context.replies.empty(), "duplicate UDP SETUP was accepted");
                require(!stream_registry::instance().find(context.stream_id), "duplicate SETUP registered source");
            }
            else
            {
                const auto record = "RECORD " + context.request_uri + " RTSP/1.0\r\nCSeq: 10\r\nSession: " + session_id + "\r\n\r\n";
                const auto record_result = input(record);
                if (scenario == "udp_incomplete")
                {
                    require(record_result < 0 && context.replies.empty(), "RECORD accepted before all tracks SETUP");
                    require(!stream_registry::instance().find(context.stream_id), "incomplete SETUP registered source");
                }
                else
                {
                    require(record_result == 0 && context.replies.size() == 1 && context.replies.front().starts_with("RTSP/1.0 200"),
                            "UDP RECORD failed after all tracks SETUP");
                    const auto registered = stream_registry::instance().find(context.stream_id);
                    require(registered && registered->tracks().size() == (two_tracks ? 2U : 1U), "RECORD did not fix and register expected tracks");
                    const auto teardown = "TEARDOWN " + context.request_uri + " RTSP/1.0\r\nCSeq: 11\r\nSession: " + session_id + "\r\n\r\n";
                    require(input(teardown) < 0 && context.replies.size() == 1 && context.replies.front().starts_with("RTSP/1.0 200"),
                            "TEARDOWN did not reply successfully and terminate the connection");
                }
            }
            const std::weak_ptr<rtsp_publish_session> lifetime = context.publisher;
            context.publisher->shutdown();
            context.publisher.reset();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((!lifetime.expired() || stream_registry::instance().find(context.stream_id)) && std::chrono::steady_clock::now() < deadline)
            {
                worker.io().run_one_for(std::chrono::milliseconds(20));
            }
            require(lifetime.expired(), "UDP publisher remained after shutdown");
            require(!stream_registry::instance().find(context.stream_id), "shutdown retained registered source");
            worker.request_stop();
            while (!worker.io().stopped() && std::chrono::steady_clock::now() < deadline)
            {
                worker.io().run_one_for(std::chrono::milliseconds(20));
            }
            require(worker.io().stopped(), "UDP shutdown retained asynchronous work after worker stop");
            auto& pool = media_port_pool::instance();
            std::vector<std::uint16_t> available_ports;
            while (const auto port = pool.acquire())
            {
                require(*port >= 24'400 && *port <= 24'406 && *port % 2U == 0 &&
                            std::find(available_ports.begin(), available_ports.end(), *port) == available_ports.end(),
                        "UDP shutdown duplicated or changed a media allocation");
                available_ports.push_back(*port);
                boost::asio::ip::udp::socket rtp(worker.io(), {boost::asio::ip::address_v4::loopback(), *port});
                boost::asio::ip::udp::socket rtcp(worker.io(), {boost::asio::ip::address_v4::loopback(), static_cast<std::uint16_t>(*port + 1U)});
            }
            require(available_ports.size() == 4U, "UDP SETUP/shutdown leaked or duplicated media allocations");
            for (auto port = available_ports.rbegin(); port != available_ports.rend(); ++port)
            {
                pool.release(*port);
            }
        }
    }
    std::cout << "RTSP publish " << scenario << ": PASS\n";
}
}    // namespace

int main()
{
    try
    {
        const auto aac = rtsp_sdp_track_from_format(
            "audio", 97, 48'000, "MPEG4-GENERIC",
            "97 streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1190;SizeLength=13;IndexLength=3;IndexDeltaLength=3", 2);
        require(aac && aac->codec == codec_id::aac && aac->clock_rate == 48'000 && aac->channel_count == 2 &&
                    aac->codec_config == std::vector<std::uint8_t>{0x11, 0x90},
                "valid AAC configuration was not normalized");
        for (const char* fmtp : std::array<const char*, 5>{nullptr, "97 mode=AAC-hbr", "97 config=119", "97 config=11zz", "97 config=0000"})
        {
            require(!rtsp_sdp_track_from_format("audio", 97, 48'000, "MPEG4-GENERIC", fmtp, 2),
                    "invalid AAC configuration was accepted");
        }
        std::cout << "RTSP AAC configuration guards: PASS\n";
        media_port_pool::init(24'400, 24'407);
        for (const auto* scenario :
             {"relative", "content_base", "content_location", "absolute", "invalid_sdp", "udp_one", "udp_two", "udp_duplicate", "udp_incomplete"})
        {
            content_urls(scenario);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "RTSP publish: " << error.what() << '\n';
        return 1;
    }
}
