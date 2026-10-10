#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/crc.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/core/media_stream.h"
#include "media/net/media_port_pool.h"
#include "media/net/udp_transport.h"
#include "media/net/worker_context.h"
#include "media/webrtc/dtls_certificate.h"
#include "media/webrtc/stun_message.h"
#include "media/webrtc/whep_session.h"

namespace
{
using namespace media_server;
using namespace std::chrono_literals;
using udp = boost::asio::ip::udp;

SSL* server_ssl{};
unsigned timeout_calls{};
unsigned retransmissions{};
unsigned exhausted{};
unsigned freed{};
bool accelerate_timer{true};

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Only this test accelerates OpenSSL's timer; its retransmission limit and failure remain real.
unsigned int short_timer(SSL*, unsigned int) { return 50'000; }

std::vector<std::uint8_t> nomination()
{
    std::vector<std::uint8_t> packet{0,    1, 0, 0,  0x21, 0x12, 0xa4, 0x42, 1,   2,   3,   4,   5,   6,   7,   8, 9, 10,   11, 12,
                                     0,    6, 0, 11, 'l',  'o',  'c',  'a',  'l', ':', 'r', 'e', 'm', 'o', 't', 0, 0, 0x24, 0,  4,
                                     0x6e, 0, 0, 1,  0x80, 0x2a, 0,    8,    0,   0,   0,   0,   0,   0,   0,   1, 0, 0x25, 0,  0};
    const auto set_length = [&]()
    {
        const auto size = packet.size() - 20;
        packet[2] = static_cast<std::uint8_t>(size >> 8U);
        packet[3] = static_cast<std::uint8_t>(size);
    };
    // MESSAGE-INTEGRITY covers the preceding bytes with the length including this attribute.
    packet[3] = static_cast<std::uint8_t>(packet.size() - 20 + 24);
    const std::string password = "local-password";
    std::array<unsigned char, 20> digest{};
    unsigned int size{};
    require(HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()), packet.data(), packet.size(), digest.data(), &size) != nullptr &&
                size == digest.size(),
            "STUN integrity failed");
    packet.insert(packet.end(), {0, 8, 0, 20});
    packet.insert(packet.end(), digest.begin(), digest.end());
    packet[3] = static_cast<std::uint8_t>(packet.size() - 20 + 8);
    boost::crc_32_type crc;
    crc.process_bytes(packet.data(), packet.size());
    const auto value = crc.checksum() ^ 0x5354554eU;
    packet.insert(packet.end(),
                  {0x80,
                   0x28,
                   0,
                   4,
                   static_cast<std::uint8_t>(value >> 24U),
                   static_cast<std::uint8_t>(value >> 16U),
                   static_cast<std::uint8_t>(value >> 8U),
                   static_cast<std::uint8_t>(value)});
    set_length();
    require(parse_stun_binding_request(packet, "local:remot", password).has_value(), "STUN nomination invalid");
    return packet;
}
}    // namespace

extern "C" SSL* __real_SSL_new(SSL_CTX*);
extern "C" void __real_SSL_free(SSL*);
extern "C" long __real_SSL_ctrl(SSL*, int, long, void*);

extern "C" SSL* __wrap_SSL_new(SSL_CTX* context)
{
    auto* ssl = __real_SSL_new(context);
    if (ssl != nullptr)
    {
        require(server_ssl == nullptr, "unexpected second server SSL");
        server_ssl = ssl;
        if (accelerate_timer)
        {
            DTLS_set_timer_cb(ssl, short_timer);
        }
    }
    return ssl;
}

extern "C" void __wrap_SSL_free(SSL* ssl)
{
    if (ssl != nullptr && ssl == server_ssl)
    {
        if (ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_READ_TIMEOUT_EXPIRED)
        {
            ++exhausted;
        }
        ++freed;
        server_ssl = nullptr;
    }
    __real_SSL_free(ssl);
}

extern "C" long __wrap_SSL_ctrl(SSL* ssl, int command, long argument, void* pointer)
{
    const auto result = __real_SSL_ctrl(ssl, command, argument, pointer);
    if (ssl == server_ssl && command == DTLS_CTRL_HANDLE_TIMEOUT)
    {
        ++timeout_calls;
        if (result > 0)
        {
            ++retransmissions;
        }
    }
    return result;
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "establishment" || std::string_view(argv[1]) == "late")),
                "invalid test scenario");
        const bool late_completion = argc == 2 && std::string_view(argv[1]) == "late";
        accelerate_timer = argc == 1 || late_completion;
        media_port_pool::init(24'802, 24'803);
        worker_context source_worker;
        worker_context viewer_worker;
        const auto address = boost::asio::ip::address_v4::loopback();
        auto source = std::make_shared<media_stream>("dtls-exhaustion", source_worker);
        require(source->set_tracks(
                    {{.id = 1, .kind = media_kind::audio, .codec = codec_id::g711a, .clock_rate = 8'000, .channel_count = 1, .codec_config = {}}}),
                "source tracks rejected");
        const auto port = media_port_pool::instance().acquire();
        require(port.has_value(), "media port unavailable");
        boost::scope::scope_exit release_port([&]() { media_port_pool::instance().release(*port); });
        auto transport = std::make_shared<udp_transport>(viewer_worker.io());
        boost::system::error_code error;
        transport->startup(address, *port, error);
        require(!error, "UDP bind failed");
        udp::socket peer(viewer_worker.io(), {address, 0});
        peer.non_blocking(true);
        const auto certificate = dtls_certificate::create();
        require(certificate != nullptr, "certificate failed");
        const auto offer = parse_webrtc_offer(
            "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=recvonly\r\n"
            "a=rtcp-mux\r\na=setup:actpass\r\na=ice-ufrag:remot\r\na=ice-pwd:remote-password\r\n"
            "a=fingerprint:sha-256 " +
            certificate->sha256_fingerprint() + "\r\na=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=rtpmap:8 PCMA/8000\r\n");
        require(offer.has_value(), "offer invalid");
        webrtc_answer_config config{.address = address,
                                    .port = *port,
                                    .stream_id = "dtls-exhaustion",
                                    .ice_ufrag = "local",
                                    .ice_pwd = "local-password",
                                    .fingerprint = certificate->sha256_fingerprint()};
        const auto answer = make_webrtc_answer(*offer, source->tracks(), config);
        require(answer.has_value(), "answer invalid");
        auto session = std::make_shared<whep_session>(viewer_worker, source, transport, std::move(config));
        release_port.set_active(false);
        boost::scope::scope_exit cleanup(
            [&]()
            {
                if (session)
                {
                    session->shutdown();
                }
                source_worker.request_stop();
                viewer_worker.request_stop();
                source_worker.io().run_for(1s);
                viewer_worker.io().run_for(1s);
            });
        const auto begin = std::chrono::steady_clock::now();
        boost::asio::post(viewer_worker.io(),
                          [&]() { require(session->startup(*find_webrtc_transport(*offer), *answer, *certificate), "startup failed"); });
        viewer_worker.io().poll();
        source_worker.io().poll();
        const std::weak_ptr<whep_session> session_lifetime = session;
        const std::weak_ptr<udp_transport> transport_lifetime = transport;
        peer.send_to(boost::asio::buffer(nomination()), {address, *port});

        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_context(SSL_CTX_new(DTLS_client_method()), SSL_CTX_free);
        require(client_context != nullptr, "client context failed");
        std::unique_ptr<SSL, decltype(&SSL_free)> client(__real_SSL_new(client_context.get()), SSL_free);
        require(client != nullptr, "client SSL failed");
        SSL_set_bio(client.get(), BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
        BIO_set_mem_eof_return(SSL_get_rbio(client.get()), -1);
        SSL_set_connect_state(client.get());
        const auto handshake = SSL_do_handshake(client.get());
        require(handshake < 0 && SSL_get_error(client.get(), handshake) == SSL_ERROR_WANT_READ, "client did not generate ClientHello");
        std::array<std::uint8_t, 4096> buffer{};
        const auto hello_size = BIO_read(SSL_get_wbio(client.get()), buffer.data(), static_cast<int>(buffer.size()));
        require(hello_size > 0, "ClientHello empty");
        const std::vector<std::uint8_t> hello(buffer.begin(), buffer.begin() + hello_size);
        peer.send_to(boost::asio::buffer(buffer.data(), static_cast<std::size_t>(hello_size)), {address, *port});
        unsigned flights{};
        unsigned stun_responses{};
        bool late_hello_sent{};
        const auto deadline = begin + (accelerate_timer ? 5s : 18s);
        do
        {
            viewer_worker.io().run_one_for(2ms);
            source_worker.io().poll();
            udp::endpoint endpoint;
            for (;;)
            {
                const auto bytes = peer.receive_from(boost::asio::buffer(buffer), endpoint, 0, error);
                if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
                {
                    break;
                }
                require(!error && endpoint.port() == *port, "unexpected UDP response");
                // Deliberately discard every server flight: no handshake reply is sent.
                if (dtls_transport::is_dtls_packet({buffer.data(), bytes}))
                {
                    ++flights;
                }
                else if (is_stun_message({buffer.data(), bytes}))
                {
                    ++stun_responses;
                }
            }
            if (late_completion && retransmissions == 12 && !late_hello_sent)
            {
                timeval remaining{};
                require(server_ssl != nullptr && DTLSv1_get_timeout(server_ssl, &remaining) == 1, "last DTLS timer missing");
                boost::asio::io_context delay_io;
                // Make the real read and final timer ready together, without running the viewer executor recursively.
                boost::asio::steady_timer expired_timer(delay_io,
                                                        std::chrono::seconds(remaining.tv_sec) + std::chrono::microseconds(remaining.tv_usec) + 20ms);
                expired_timer.async_wait([](boost::system::error_code timer_error) { require(!timer_error, "late-hello barrier failed"); });
                delay_io.run();
                for (unsigned replay = 0; replay != 3; ++replay)
                {
                    peer.send_to(boost::asio::buffer(hello), {address, *port});
                }
                late_hello_sent = true;
            }
            static_cast<void>(transport->local_endpoint(error));
        } while (!error && std::chrono::steady_clock::now() < deadline);
        require(retransmissions > 1 && flights > 1 && stun_responses == 1, "real DTLS retransmissions not observed");
        require(exhausted == (accelerate_timer ? 1U : 0U), "unexpected DTLS exhaustion result");
        require(late_hello_sent == late_completion, "late ClientHello injection missing");
        if (!accelerate_timer)
        {
            require(std::chrono::steady_clock::now() - begin >= 15s, "WHEP closed before its establishment deadline");
        }
        require(error == boost::asio::error::bad_descriptor && freed == 1, "DTLS failure did not release SSL and UDP socket");
        source_worker.io().poll();
        viewer_worker.io().poll();
        const media_frame frame{.track = 1,
                                .dts_ns = 20'000'000,
                                .pts_ns = 20'000'000,
                                .key_frame = false,
                                .payload = std::make_shared<const std::vector<std::uint8_t>>(160, 0xd5)};
        boost::asio::post(source_worker.io(), [source, frame]() { source->publish(frame); });
        source_worker.io().poll();
        require(viewer_worker.io().poll() == 0, "closed WHEP sink still queued media delivery");
        const auto reacquired = media_port_pool::instance().acquire();
        require(reacquired == port, "media port not returned");
        boost::scope::scope_exit release_reacquired([&]() { media_port_pool::instance().release(*reacquired); });
        udp::socket rebound(viewer_worker.io(), {address, *reacquired});
        session->shutdown();
        session->shutdown();
        boost::asio::post(viewer_worker.io(),
                          [current = session, frame]()
                          {
                              current->on_frame(frame);
                              current->on_end();
                          });
        viewer_worker.io().poll();
        require(!media_port_pool::instance().acquire() && freed == 1, "repeat cleanup freed another owner's resources");
        session.reset();
        transport.reset();
        source_worker.io().poll();
        viewer_worker.io().poll();
        require(session_lifetime.expired() && transport_lifetime.expired(), "DTLS failure retained session or transport");
        std::cout << "scenario="
                  << (late_completion    ? "late"
                      : accelerate_timer ? "exhaustion"
                                         : "establishment")
                  << " DTLS exhausted=" << exhausted << " retransmissions=" << retransmissions << " timeout_calls=" << timeout_calls
                  << " UDP_flights=" << flights << " SSL_freed=" << freed
                  << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count()
                  << " late_hello_sent=" << late_hello_sent << " sink_removed=1 port_returned_once=1 weak_expired=1\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WHEP DTLS timeout: " << error.what() << '\n';
        return 1;
    }
}
