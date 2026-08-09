#include "media/codec/codec_utils.h"
#include "media/core/media_sink.h"
#include "media/core/media_stream.h"
#include "media/core/stream_registry.h"
#include "media/hls/hls_output.h"
#include "media/hls/hls_service.h"
#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/http/http_flv_output.h"
#include "media/rtmp/rtmp_timestamp.h"
#include "media/webrtc/webrtc_output.h"

extern "C"
{
#include "flv-proto.h"
#include "rtsp-client.h"
#include "rtsp-muxer.h"
#include "rtsp-payloads.h"
}

#include <algorithm>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace media_server
{
namespace
{

constexpr track_id video_track_id = 1;
constexpr track_id audio_track_id = 2;

const std::vector<std::uint8_t> h264_config{
    0x00, 0x00, 0x00, 0x01,
    0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6e, 0x40,
    0x00, 0x00, 0x00, 0x01,
    0x68, 0xce, 0x3c, 0x80,
};

const std::vector<std::uint8_t> h265_config{
    0x00, 0x00, 0x00, 0x01,
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x78, 0x9d, 0xc0, 0x90,
    0x00, 0x00, 0x00, 0x01,
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x32, 0x16, 0x59, 0xde, 0x49, 0x1b, 0x6b, 0x80, 0x40, 0x00,
    0x00, 0xfa, 0x00, 0x00, 0x17, 0x70, 0x02,
    0x00, 0x00, 0x00, 0x01,
    0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89,
};

const std::vector<std::uint8_t> aac_asc{0x12, 0x10};

const std::vector<std::vector<std::uint8_t>> valid_aac_adts_frames{
    {0xff, 0xf1, 0x50, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61, 0x76, 0x63, 0x36, 0x31,
     0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00, 0x42, 0x20, 0x08, 0xc1, 0x18, 0x38},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
    {0xff, 0xf1, 0x50, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c},
};

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "[fail] " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

media_track make_video_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h264,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h264_config,
    };
}

media_track make_h265_track()
{
    return media_track{
        .id = video_track_id,
        .kind = media_kind::video,
        .codec = codec_id::h265,
        .clock_rate = 90'000,
        .channel_count = 0,
        .codec_config = h265_config,
    };
}

media_track make_audio_track()
{
    return media_track{
        .id = audio_track_id,
        .kind = media_kind::audio,
        .codec = codec_id::aac,
        .clock_rate = 44'100,
        .channel_count = 2,
        .codec_config = aac_asc,
    };
}

media_track make_video_track(std::uint8_t config_marker)
{
    auto track = make_video_track();
    track.codec_config.push_back(config_marker);
    return track;
}

media_track make_audio_track(std::uint8_t config_marker)
{
    auto track = make_audio_track();
    track.codec_config.push_back(config_marker);
    return track;
}


media_frame make_video_frame(std::int64_t pts_ns, bool key_frame)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes = h264_config;
        const std::vector<std::uint8_t> idr{
            0x00, 0x00, 0x00, 0x01,
            0x65, 0x88, 0x84, 0x21, 0xa0,
        };
        bytes.insert(bytes.end(), idr.begin(), idr.end());
    }
    else
    {
        bytes = {
            0x00, 0x00, 0x00, 0x01,
            0x41, 0x9a, 0x22, 0x11,
        };
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = key_frame,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)),
    };
}

media_frame make_h265_frame(std::int64_t pts_ns, bool key_frame)
{
    std::vector<std::uint8_t> bytes;
    if (key_frame)
    {
        bytes = h265_config;
        const std::vector<std::uint8_t> idr{
            0x00, 0x00, 0x00, 0x01,
            0x26, 0x01, 0x9a, 0x20, 0x11, 0x00,
        };
        bytes.insert(bytes.end(), idr.begin(), idr.end());
    }
    else
    {
        bytes = {
            0x00, 0x00, 0x00, 0x01,
            0x02, 0x01, 0x9a, 0x20,
        };
    }
    return media_frame{
        .track = video_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = key_frame,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)),
    };
}

media_frame make_audio_frame(std::int64_t pts_ns)
{
    const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
    auto adts = make_adts_frame(aac_asc, raw);
    require(!adts.empty(), "make adts");
    return media_frame{
        .track = audio_track_id,
        .dts_ns = pts_ns,
        .pts_ns = pts_ns,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(adts)),
    };
}

class counting_sink final : public media_sink
{
public:
    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};
};

class self_removing_sink final : public media_sink
{
public:
    explicit self_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.remove_sink(*this);
    }
    void on_end() override {}

    std::size_t frames{};

private:
    media_stream& stream_;
};

class track_removing_sink final : public media_sink
{
public:
    explicit track_removing_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(*this);
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class track_other_removing_sink final : public media_sink
{
public:
    track_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.remove_sink(target_);
    }
    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::size_t tracks{};

private:
    media_stream& stream_;
    media_sink& target_;
};

class frame_other_removing_sink final : public media_sink
{
public:
    frame_other_removing_sink(media_stream& stream, media_sink& target) : stream_(stream), target_(target) {}

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.remove_sink(target_);
    }
    void on_end() override {}

    std::size_t frames{};

private:
    media_stream& stream_;
    media_sink& target_;
};

class track_ending_sink final : public media_sink
{
public:
    explicit track_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override
    {
        ++tracks;
        stream_.end();
    }
    void on_frame(const media_frame&) override { ++frames; }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class frame_ending_sink final : public media_sink
{
public:
    explicit frame_ending_sink(media_stream& stream) : stream_(stream) {}

    void on_track(const media_track&) override { ++tracks; }
    void on_frame(const media_frame&) override
    {
        ++frames;
        stream_.end();
    }
    void on_end() override { ++ends; }

    std::size_t tracks{};
    std::size_t frames{};
    std::size_t ends{};

private:
    media_stream& stream_;
};

class track_updating_sink final : public media_sink
{
public:
    track_updating_sink(
        media_stream& stream,
        track_id trigger_id,
        std::uint64_t trigger_version,
        media_track replacement)
        : stream_(stream),
          trigger_id_(trigger_id),
          trigger_version_(trigger_version),
          replacement_(std::move(replacement))
    {
    }

    void on_track(const media_track& track) override
    {
        versions.emplace_back(track.id, track.config_version);
        if (!updated && track.id == trigger_id_ && track.config_version == trigger_version_)
        {
            updated = true;
            update_succeeded = stream_.update_track(replacement_);
        }
    }

    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::vector<std::pair<track_id, std::uint64_t>> versions;
    bool updated{};
    bool update_succeeded{};

private:
    media_stream& stream_;
    track_id trigger_id_{};
    std::uint64_t trigger_version_{};
    media_track replacement_;
};

class track_version_sink final : public media_sink
{
public:
    void on_track(const media_track& track) override
    {
        versions.emplace_back(track.id, track.config_version);
    }

    void on_frame(const media_frame&) override {}
    void on_end() override {}

    std::vector<std::pair<track_id, std::uint64_t>> versions;
};

class generation_replacing_sink final : public media_sink
{
public:
    generation_replacing_sink(stream_registry& registry, std::shared_ptr<media_stream> replacement)
        : registry_(registry), replacement_(std::move(replacement))
    {
    }

    void on_track(const media_track&) override {}
    void on_frame(const media_frame&) override {}
    void on_end() override
    {
        old_generation_hidden = !registry_.find(replacement_->name());
        replacement_added = registry_.add(replacement_);
    }

    bool old_generation_hidden{};
    bool replacement_added{};

private:
    stream_registry& registry_;
    std::shared_ptr<media_stream> replacement_;
};

struct rtp_timeline_capture
{
    std::vector<std::uint32_t> timestamps;
};

struct rtsp_client_capture
{
    std::string request;
    int setup_timeout{-1};
};

int capture_rtsp_request(void* param, const char*, const void* request, std::size_t bytes)
{
    auto& capture = *static_cast<rtsp_client_capture*>(param);
    capture.request.assign(static_cast<const char*>(request), bytes);
    return static_cast<int>(bytes);
}

int capture_rtsp_rtp_port(
    void*,
    int media,
    const char*,
    unsigned short port[2],
    char*,
    int)
{
    port[0] = static_cast<unsigned short>(media * 2);
    port[1] = static_cast<unsigned short>(media * 2 + 1);
    return RTSP_TRANSPORT_RTP_TCP;
}

int capture_rtsp_setup(void* param, int timeout, std::int64_t)
{
    static_cast<rtsp_client_capture*>(param)->setup_timeout = timeout;
    return 0;
}

int rtsp_setup_timeout(std::string_view session_header)
{
    rtsp_client_capture capture;
    rtsp_client_handler_t handler{};
    handler.send = &capture_rtsp_request;
    handler.rtpport = &capture_rtsp_rtp_port;
    handler.onsetup = &capture_rtsp_setup;

    auto* client = rtsp_client_create(
        "rtsp://127.0.0.1/live/test",
        nullptr,
        nullptr,
        &handler,
        &capture);
    require(client != nullptr, "rtsp client create");

    constexpr std::string_view sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";
    require(
        rtsp_client_setup(client, sdp.data(), static_cast<int>(sdp.size())) == 0,
        "rtsp client setup request");

    const auto response = std::string("RTSP/1.0 200 OK\r\n") +
        "CSeq: 1\r\n" +
        "Session: " + std::string(session_header) + "\r\n" +
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" +
        "Content-Length: 0\r\n\r\n";
    require(rtsp_client_input(client, response.data(), response.size()) == 0, "rtsp setup response");

    require(rtsp_client_options(client, nullptr) == 0, "rtsp keepalive options");
    require(capture.request.starts_with("OPTIONS * RTSP/1.0\r\n"), "rtsp keepalive method");
    const auto separator = session_header.find(';');
    const auto session_id = session_header.substr(0, separator);
    require(
        capture.request.find("Session: " + std::string(session_id) + "\r\n") != std::string::npos,
        "rtsp keepalive session");

    const auto timeout = capture.setup_timeout;
    rtsp_client_destroy(client);
    return timeout;
}

void test_rtsp_client_session_timeout()
{
    require(rtsp_setup_timeout("session-1;timeout=70") == 70, "rtsp setup explicit timeout");
    require(rtsp_setup_timeout("session-2") == 60, "rtsp setup default timeout");
}

int ignore_rtsp_media(
    void*,
    int,
    const char*,
    unsigned short[2],
    char*,
    int)
{
    return 0;
}

void test_rtsp_client_rejects_empty_media_selection()
{
    rtsp_client_capture capture;
    rtsp_client_handler_t handler{};
    handler.send = &capture_rtsp_request;
    handler.rtpport = &ignore_rtsp_media;

    auto* client = rtsp_client_create(
        "rtsp://127.0.0.1/live/test",
        nullptr,
        nullptr,
        &handler,
        &capture);
    require(client != nullptr, "rtsp empty selection client create");

    constexpr std::string_view sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H265/90000\r\n"
        "a=control:trackID=0\r\n";
    require(
        rtsp_client_setup(client, sdp.data(), static_cast<int>(sdp.size())) != 0,
        "rtsp reject all ignored media");
    require(capture.request.empty(), "rtsp ignored media sends no setup");
    rtsp_client_destroy(client);
}

std::size_t rtsp_content_length(std::string_view response)
{
    constexpr std::string_view name = "Content-Length:";
    const auto offset = response.find(name);
    if (offset == std::string_view::npos)
    {
        return 0;
    }
    auto begin = offset + name.size();
    while (begin < response.size() && response[begin] == ' ')
    {
        ++begin;
    }
    const auto end = response.find("\r\n", begin);
    require(end != std::string_view::npos, "rtsp response content length end");
    std::size_t length = 0;
    const auto [pointer, error] = std::from_chars(response.data() + begin, response.data() + end, length);
    require(error == std::errc{} && pointer == response.data() + end, "rtsp response content length");
    return length;
}

std::string rtsp_header_value(std::string_view response, std::string_view name)
{
    const auto offset = response.find(name);
    if (offset == std::string_view::npos)
    {
        return {};
    }
    auto begin = offset + name.size();
    while (begin < response.size() && response[begin] == ' ')
    {
        ++begin;
    }
    const auto end = response.find("\r\n", begin);
    require(end != std::string_view::npos, "rtsp response header end");
    return std::string(response.substr(begin, end - begin));
}

class rtsp_output_test_peer final
{
public:
    explicit rtsp_output_test_peer(bool h265 = false)
        : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}), client_(io_)
    {
        stream_ = std::make_shared<media_stream>("live/test");
        require(stream_->update_track(h265 ? make_h265_track() : make_video_track()), "rtsp output video track");
        require(stream_->update_track(make_audio_track()), "rtsp output audio track");
        require(registry_.add(stream_), "rtsp output registry add");

        client_.connect(acceptor_.local_endpoint());
        auto server_socket = acceptor_.accept();
        auto connection = std::make_shared<tcp_connection>(std::move(server_socket));
        session_ = std::make_shared<rtsp_output_session>(
            std::move(connection), registry_, acceptor_.local_endpoint().port());
        session_->start();
        runner_ = std::jthread([this]() { io_.run(); });
    }

    ~rtsp_output_test_peer()
    {
        boost::system::error_code error;
        client_.close(error);
        io_.stop();
    }

    std::string request(std::string_view request)
    {
        boost::asio::write(client_, boost::asio::buffer(request));
        std::string response;
        boost::asio::read_until(client_, boost::asio::dynamic_buffer(response), "\r\n\r\n");
        const auto header_end = response.find("\r\n\r\n");
        require(header_end != std::string::npos, "rtsp response header");
        const auto total = header_end + 4U + rtsp_content_length(response);
        if (response.size() < total)
        {
            boost::asio::read(
                client_,
                boost::asio::dynamic_buffer(response),
                boost::asio::transfer_exactly(total - response.size()));
        }
        return response;
    }

    [[nodiscard]] std::uint16_t port() const
    {
        return acceptor_.local_endpoint().port();
    }

    [[nodiscard]] std::shared_ptr<media_stream> stream() const
    {
        return stream_;
    }

private:
    boost::asio::io_context io_;
    stream_registry registry_;
    std::shared_ptr<media_stream> stream_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket client_;
    std::shared_ptr<rtsp_output_session> session_;
    std::jthread runner_;
};

void test_rtsp_output_session_contract()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";

    const auto describe = peer.request(
        "DESCRIBE " + base + " RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp output describe");
    require(describe.find("a=control:trackID=1\r\n") != std::string::npos, "rtsp output video control");
    require(describe.find("a=control:trackID=2\r\n") != std::string::npos, "rtsp output audio control");

    const auto wrong_stream = peer.request(
        "SETUP rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/other/trackID=1 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(wrong_stream.starts_with("RTSP/1.0 404"), "rtsp output setup stream identity");

    const auto video_setup = peer.request(
        "SETUP " + base + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(video_setup.starts_with("RTSP/1.0 200"), "rtsp output video setup");
    const auto session = rtsp_header_value(video_setup, "Session:");
    require(!session.empty(), "rtsp output session id");

    const auto duplicate_setup = peer.request(
        "SETUP " + base + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Session: " + session + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(duplicate_setup.starts_with("RTSP/1.0 200"), "rtsp output idempotent setup");

    const auto wrong_session = peer.request(
        "SETUP " + base + "/trackID=2 RTSP/1.0\r\n"
        "CSeq: 5\r\n"
        "Session: wrong\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(wrong_session.starts_with("RTSP/1.0 454"), "rtsp output setup session identity");

    const auto channel_conflict = peer.request(
        "SETUP " + base + "/trackID=2 RTSP/1.0\r\n"
        "CSeq: 6\r\n"
        "Session: " + session + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=1-2\r\n\r\n");
    require(channel_conflict.starts_with("RTSP/1.0 461"), "rtsp output interleaved channel conflict");

    const auto audio_setup = peer.request(
        "SETUP " + base + "/trackID=2 RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "Session: " + session + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    require(audio_setup.starts_with("RTSP/1.0 200"), "rtsp output audio setup");

    const auto wrong_play = peer.request(
        "PLAY rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/other RTSP/1.0\r\n"
        "CSeq: 8\r\n"
        "Session: " + session + "\r\n\r\n");
    require(wrong_play.starts_with("RTSP/1.0 404"), "rtsp output play stream identity");

    const auto play = peer.request(
        "PLAY " + base + " RTSP/1.0\r\n"
        "CSeq: 9\r\n"
        "Session: " + session + "\r\n\r\n");
    require(play.starts_with("RTSP/1.0 200"), "rtsp output play");

    const auto late_setup = peer.request(
        "SETUP " + base + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: 10\r\n"
        "Session: " + session + "\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(late_setup.starts_with("RTSP/1.0 455"), "rtsp output reject setup after play");
}

void test_rtsp_output_h265()
{
    rtsp_output_test_peer peer(true);
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request(
        "DESCRIBE " + base + " RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp h265 describe");
    require(describe.find("H265/90000") != std::string::npos, "rtsp h265 rtpmap");
    require(describe.find("sprop-vps=") != std::string::npos, "rtsp h265 vps");
    require(describe.find("sprop-sps=") != std::string::npos, "rtsp h265 sps");
    require(describe.find("sprop-pps=") != std::string::npos, "rtsp h265 pps");
}

void test_rtsp_output_rejects_stale_description()
{
    rtsp_output_test_peer peer;
    const auto base = "rtsp://127.0.0.1:" + std::to_string(peer.port()) + "/live/test";
    const auto describe = peer.request(
        "DESCRIBE " + base + " RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n\r\n");
    require(describe.starts_with("RTSP/1.0 200"), "rtsp stale describe");

    auto updated = make_video_track();
    updated.codec_config.push_back(0x01);
    require(peer.stream()->update_track(std::move(updated)), "rtsp stale source config update");

    const auto setup = peer.request(
        "SETUP " + base + "/trackID=1 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    require(setup.starts_with("RTSP/1.0 455"), "rtsp reject stale described config");
}

int capture_rtp_timestamp(
    void* param,
    int,
    const void*,
    int bytes,
    std::uint32_t timestamp,
    int)
{
    if (bytes > 0)
    {
        static_cast<rtp_timeline_capture*>(param)->timestamps.push_back(timestamp);
    }
    return 0;
}

void test_timebase_conversions()
{
    constexpr std::int64_t thirty_hours_ns = 30LL * 60 * 60 * 1'000'000'000;
    require(ns_to_90khz(thirty_hours_ns) == 9'720'000'000LL, "90khz long timeline");

    constexpr std::int64_t long_milliseconds =
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1'234;
    const auto long_ns = milliseconds_to_ns(long_milliseconds);
    require(ns_to_milliseconds(long_ns) == long_milliseconds, "millisecond timeline keeps int64 range");
    require(ns_to_flv_milliseconds(long_ns) == 1'233U, "flv timestamp wraps uint32 timeline");

    constexpr std::int64_t negative_ns = -40'000'000;
    require(ns_to_milliseconds(negative_ns) == -40, "millisecond timeline keeps negative values");
    require(ns_to_flv_milliseconds(negative_ns) == std::numeric_limits<std::uint32_t>::max() - 39U,
            "flv timestamp keeps signed composition offset modulo uint32");
    require(ns_to_90khz(negative_ns) == -3'600, "90khz timeline keeps negative values");
}

void test_rtmp_timestamp_timeline()
{
    rtmp_timestamp_state state;
    const auto near_wrap = std::numeric_limits<std::uint32_t>::max() - 9U;
    require(unwrap_rtmp_timestamp(near_wrap, state) == static_cast<std::int64_t>(near_wrap),
            "rtmp timestamp initial value");
    require(unwrap_rtmp_timestamp(5U, state) == static_cast<std::int64_t>(near_wrap) + 15,
            "rtmp timestamp unwraps uint32 wrap");
    require(unwrap_rtmp_timestamp(45U, state) == static_cast<std::int64_t>(near_wrap) + 55,
            "rtmp timestamp continues after wrap");

    require(rtmp_timestamp_delta(960U, 1'000U) == -40, "rtmp signed negative composition offset");
    require(rtmp_timestamp_delta(1'040U, 1'000U) == 40, "rtmp signed positive composition offset");
}

void test_internal_format_contract()
{
    const auto avcc = h264_annex_b_to_avcc(h264_config);
    require(!avcc.empty(), "annex-b to avcc");
    const auto annex_b = h264_avcc_to_annex_b(avcc);
    require(!annex_b.empty(), "avcc to annex-b");
    require(annex_b.size() >= 8, "annex-b config size");
    require(annex_b[0] == 0 && annex_b[1] == 0 && annex_b[2] == 0 && annex_b[3] == 1,
            "annex-b four-byte start code");

    const auto hvcc = h265_annex_b_to_hvcc(h265_config);
    require(!hvcc.empty(), "h265 annex-b to hvcc");
    const auto hevc_annex_b = h265_hvcc_to_annex_b(hvcc);
    require(!hevc_annex_b.empty(), "h265 hvcc to annex-b");
    require(hevc_annex_b.size() >= 8U, "h265 annex-b config size");
    require(hevc_annex_b[0] == 0 && hevc_annex_b[1] == 0 && hevc_annex_b[2] == 0 && hevc_annex_b[3] == 1,
            "h265 annex-b four-byte start code");

    const std::vector<std::uint8_t> raw{0x11, 0x22, 0x33, 0x44};
    const auto adts = make_adts_frame(aac_asc, raw);
    require(adts.size() > raw.size(), "adts header exists");
    const auto aac = parse_aac_adts(adts);
    require(aac.has_value(), "parse adts");
    require(aac->sample_rate == 44'100 && aac->channel_count == 2, "adts aac config");
}

void test_flv_config_cache_lifecycle()
{
    std::size_t video_sequence_headers = 0;
    std::size_t audio_sequence_headers = 0;
    std::optional<std::int32_t> video_composition_time;
    std::optional<std::uint32_t> video_timestamp;
    flv_output_muxer output(
        [&video_sequence_headers, &audio_sequence_headers, &video_composition_time, &video_timestamp](
            int type,
            std::span<const std::uint8_t> data,
            std::uint32_t timestamp) {
            if (data.size() < 2U)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO && data[1] == 1U && data.size() >= 5U)
            {
                const auto raw =
                    (static_cast<std::uint32_t>(data[2]) << 16U) |
                    (static_cast<std::uint32_t>(data[3]) << 8U) |
                    static_cast<std::uint32_t>(data[4]);
                video_composition_time = raw < 0x00800000U
                    ? static_cast<std::int32_t>(raw)
                    : static_cast<std::int32_t>(static_cast<std::int64_t>(raw) - 0x01000000LL);
                video_timestamp = timestamp;
                return;
            }
            if (data[1] != 0U)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                ++video_sequence_headers;
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                ++audio_sequence_headers;
            }
        });

    auto video = make_video_track();
    video.config_version = 1;
    output.on_track(video);
    require(video_sequence_headers == 1U, "flv initial video sequence header");
    auto reordered_video = make_video_frame(-40'000'000, true);
    reordered_video.dts_ns = 0;
    output.on_frame(reordered_video);
    require(video_timestamp == 0U, "flv video dts stays on unsigned tag timeline");
    require(video_composition_time == -40, "flv video keeps negative composition time");

    auto audio = make_audio_track();
    audio.config_version = 1;
    output.on_track(audio);
    output.on_frame(make_audio_frame(0));
    require(audio_sequence_headers == 1U, "flv initial audio sequence header");

    const std::vector<std::uint8_t> updated_asc{0x11, 0x90};
    auto updated_audio = audio;
    updated_audio.clock_rate = 48'000;
    updated_audio.codec_config = updated_asc;
    updated_audio.config_version = 2;
    output.on_track(updated_audio);
    require(video_sequence_headers == 2U, "flv config generation resets cached video header");

    const std::vector<std::uint8_t> raw{0x21, 0x10, 0x56, 0xe5, 0x00, 0x11, 0x22, 0x33};
    auto updated_adts = make_adts_frame(updated_asc, raw);
    require(!updated_adts.empty(), "flv updated aac adts");
    output.on_frame(media_frame{
        .track = audio_track_id,
        .dts_ns = 1'000'000'000,
        .pts_ns = 1'000'000'000,
        .key_frame = false,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(updated_adts)),
    });
    require(audio_sequence_headers == 2U, "flv config generation resets cached audio header");
}

void test_h265_output_paths()
{
    std::size_t hevc_sequence_headers = 0;
    flv_output_muxer flv([&hevc_sequence_headers](int type, std::span<const std::uint8_t> data, std::uint32_t) {
        if (type == FLV_TYPE_VIDEO && data.size() >= 2U && (data[0] & 0x0fU) == FLV_VIDEO_H265 && data[1] == 0U)
        {
            ++hevc_sequence_headers;
        }
    });
    auto hevc_track = make_h265_track();
    hevc_track.config_version = 1;
    flv.on_track(hevc_track);
    require(hevc_sequence_headers == 1U, "flv h265 sequence header");
    flv.on_frame(make_h265_frame(0, true));

    hls_output hls(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    hls.on_track(make_h265_track());
    hls.on_frame(make_h265_frame(0, true));
    hls.on_frame(make_h265_frame(1'000'000'000, true));
    hls.on_end();
    require(hls.segment_count() >= 1U, "hls h265 segment");
    const auto segment = hls.segment(0);
    require(segment.has_value() && !segment->empty() && segment->size() % 188U == 0U, "hls h265 ts");

    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output webrtc(
        webrtc_output_config{
            .video_codec = codec_id::h265,
            .video_payload_type = 103,
            .rtcp_cname = {},
        },
        [&packets](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
        });
    webrtc.on_track(make_h265_track());
    require(webrtc.valid(), "webrtc h265 output valid");
    webrtc.on_frame(make_h265_frame(0, true));
    require(!packets.empty() && packets.front().size() >= 12U, "webrtc h265 rtp packet");
    require((packets.front()[1] & 0x7fU) == 103U, "webrtc h265 payload type");
}

void test_rtsp_muxer_zero_origin_timeline()
{
    rtp_timeline_capture capture;
    auto* muxer = rtsp_muxer_create(&capture_rtp_timestamp, &capture);
    require(muxer != nullptr, "rtsp muxer create");

    const auto payload = rtsp_muxer_add_payload(
        muxer, "RTP/AVP", 90'000, 96, "H264", 0, 0x12345678U, 0, nullptr, 0);
    require(payload >= 0, "rtsp muxer add payload");
    const auto media = rtsp_muxer_add_media(muxer, payload, RTP_PAYLOAD_H264, nullptr, 0);
    require(media >= 0, "rtsp muxer add media");

    const std::array<std::uint8_t, 8> frame{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11};
    require(rtsp_muxer_input(muxer, media, 0, 0, frame.data(), static_cast<int>(frame.size()), 0) == 0,
            "rtsp muxer first frame");
    require(rtsp_muxer_input(muxer, media, 40, 40, frame.data(), static_cast<int>(frame.size()), 0) == 0,
            "rtsp muxer second frame");
    require(capture.timestamps.size() == 2U, "rtsp muxer packet count");
    require(capture.timestamps[1] - capture.timestamps[0] == 3'600U, "rtsp muxer zero origin timestamp step");

    rtsp_muxer_destroy(muxer);
}

void test_media_stream_fanout_and_reentrancy()
{
    media_stream stream("live/test");
    require(stream.update_track(make_video_track()), "add video track");

    auto first = std::make_shared<counting_sink>();
    auto removing = std::make_shared<self_removing_sink>(stream);
    auto second = std::make_shared<counting_sink>();

    require(stream.add_sink(first), "add first sink");
    require(stream.add_sink(removing), "add removing sink");
    require(stream.add_sink(second), "add second sink");
    require(first->tracks == 1 && second->tracks == 1, "late sink track snapshot");

    require(stream.publish(make_video_frame(0, true)), "publish first frame");
    require(removing->frames == 1, "self-removing sink receives first frame");
    require(first->frames == 1 && second->frames == 1, "other sinks survive reentrant remove");

    require(stream.publish(make_video_frame(40'000'000, false)), "publish second frame");
    require(removing->frames == 1, "removed sink not called again");
    require(first->frames == 2 && second->frames == 2, "remaining sinks continue");

    stream.end();
    require(first->ends == 1 && second->ends == 1, "stream end fanout");

    media_stream track_remove_stream("live/track-remove");
    require(track_remove_stream.update_track(make_video_track()), "track remove video");
    require(track_remove_stream.update_track(make_audio_track()), "track remove audio");
    auto track_removing = std::make_shared<track_removing_sink>(track_remove_stream);
    require(!track_remove_stream.add_sink(track_removing), "track callback removes pending sink");
    require(track_removing->tracks == 1, "removed sink stops track replay");
    require(track_remove_stream.publish(make_video_frame(0, true)), "track remove publish");
    require(track_removing->frames == 0, "track removed sink receives no frame");

    media_stream track_end_stream("live/track-end");
    require(track_end_stream.update_track(make_video_track()), "track end video");
    require(track_end_stream.update_track(make_audio_track()), "track end audio");
    auto track_ending = std::make_shared<track_ending_sink>(track_end_stream);
    require(!track_end_stream.add_sink(track_ending), "track callback ends stream");
    require(track_end_stream.ended(), "track callback stream ended");
    require(track_ending->tracks == 1, "ended stream stops track replay");
    require(track_ending->ends == 1, "new sink receives end during track replay");

    media_stream update_remove_stream("live/update-remove");
    auto update_removed = std::make_shared<counting_sink>();
    auto update_removing = std::make_shared<track_other_removing_sink>(update_remove_stream, *update_removed);
    require(update_remove_stream.add_sink(update_removing), "add update removing sink");
    require(update_remove_stream.add_sink(update_removed), "add update removed sink");
    require(update_remove_stream.update_track(make_video_track()), "update removes later sink");
    require(update_removing->tracks == 1, "update removing sink receives track");
    require(update_removed->tracks == 0, "update removed sink skips current track");

    media_stream publish_remove_stream("live/publish-remove");
    require(publish_remove_stream.update_track(make_video_track()), "publish remove video");
    auto publish_removed = std::make_shared<counting_sink>();
    auto publish_removing = std::make_shared<frame_other_removing_sink>(publish_remove_stream, *publish_removed);
    require(publish_remove_stream.add_sink(publish_removing), "add publish removing sink");
    require(publish_remove_stream.add_sink(publish_removed), "add publish removed sink");
    require(publish_remove_stream.publish(make_video_frame(0, true)), "publish removes later sink");
    require(publish_removing->frames == 1, "publish removing sink receives frame");
    require(publish_removed->frames == 0, "publish removed sink skips current frame");

    media_stream update_end_stream("live/update-end");
    auto update_ending = std::make_shared<track_ending_sink>(update_end_stream);
    auto update_after = std::make_shared<counting_sink>();
    require(update_end_stream.add_sink(update_ending), "add update ending sink");
    require(update_end_stream.add_sink(update_after), "add update trailing sink");
    require(update_end_stream.update_track(make_video_track()), "update callback ends stream");
    require(update_end_stream.ended(), "update callback stream ended");
    require(update_ending->tracks == 1 && update_ending->ends == 1, "update ending sink lifecycle");
    require(update_after->tracks == 0 && update_after->ends == 1, "update stops callbacks after end");

    media_stream publish_end_stream("live/publish-end");
    require(publish_end_stream.update_track(make_video_track()), "publish end video");
    auto frame_ending = std::make_shared<frame_ending_sink>(publish_end_stream);
    auto frame_after = std::make_shared<counting_sink>();
    require(publish_end_stream.add_sink(frame_ending), "add frame ending sink");
    require(publish_end_stream.add_sink(frame_after), "add frame trailing sink");
    require(publish_end_stream.publish(make_video_frame(0, true)), "frame callback ends stream");
    require(publish_end_stream.ended(), "frame callback stream ended");
    require(frame_ending->frames == 1 && frame_ending->ends == 1, "frame ending sink lifecycle");
    require(frame_after->frames == 0 && frame_after->ends == 1, "publish stops callbacks after end");

    media_stream version_stream("live/version");
    auto first_version = make_video_track();
    first_version.config_version = 99;
    require(version_stream.update_track(std::move(first_version)), "version first track");
    require(version_stream.tracks().front().config_version == 1, "stream owns initial config version");
    auto version_observer = std::make_shared<track_version_sink>();
    require(version_stream.add_sink(version_observer), "version observer add");
    require(!version_stream.update_track(make_video_track()), "identical config ignored");
    require(
        version_observer->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}},
        "identical config not fanned out");
    auto changed_kind = make_video_track(2);
    changed_kind.kind = media_kind::audio;
    require(!version_stream.update_track(std::move(changed_kind)), "track kind change rejected");
    auto changed_codec = make_video_track(2);
    changed_codec.codec = codec_id::aac;
    require(!version_stream.update_track(std::move(changed_codec)), "track codec change rejected");
    auto second_version = make_video_track(2);
    second_version.config_version = 99;
    require(version_stream.update_track(std::move(second_version)), "changed config accepted");
    require(version_stream.tracks().front().config_version == 2, "stream increments config version");
    require(!version_stream.update_track(make_video_track(2)), "changed config duplicate ignored");

    media_stream update_reentrant_stream("live/update-reentrant");
    require(update_reentrant_stream.update_track(make_video_track()), "reentrant first track");
    auto update_reentrant = std::make_shared<track_updating_sink>(
        update_reentrant_stream, video_track_id, 2, make_video_track(3));
    auto update_observer = std::make_shared<track_version_sink>();
    require(update_reentrant_stream.add_sink(update_reentrant), "reentrant updater add");
    require(update_reentrant_stream.add_sink(update_observer), "reentrant observer add");
    require(update_reentrant_stream.update_track(make_video_track(2)), "reentrant outer update");
    require(update_reentrant->update_succeeded, "reentrant nested update");
    require(
        update_observer->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {video_track_id, 3}},
        "reentrant stale update skipped");

    media_stream replay_reentrant_stream("live/replay-reentrant");
    require(replay_reentrant_stream.update_track(make_video_track()), "replay video track");
    require(replay_reentrant_stream.update_track(make_audio_track()), "replay audio track");
    auto replay_reentrant = std::make_shared<track_updating_sink>(
        replay_reentrant_stream, video_track_id, 1, make_audio_track(2));
    require(replay_reentrant_stream.add_sink(replay_reentrant), "replay reentrant sink add");
    require(replay_reentrant->update_succeeded, "replay nested update");
    require(
        replay_reentrant->versions == std::vector<std::pair<track_id, std::uint64_t>>{{video_track_id, 1}, {audio_track_id, 2}},
        "replay stale track skipped");
}


void test_stream_registry_generation_lifecycle()
{
    stream_registry registry;

    auto first = std::make_shared<media_stream>("live/generation");
    auto second = std::make_shared<media_stream>("live/generation");
    require(registry.add(first), "registry first generation add");
    require(!registry.add(second), "registry active generation duplicate reject");
    require(registry.find("live/generation").get() == first.get(), "registry first generation remains");

    auto replacing = std::make_shared<generation_replacing_sink>(registry, second);
    require(first->add_sink(replacing), "registry generation replacement observer add");
    first->end();
    require(replacing->old_generation_hidden, "registry ended generation hidden during end callback");
    require(replacing->replacement_added, "registry ended generation replace during end callback");
    require(registry.find("live/generation").get() == second.get(), "registry replacement generation visible");
    registry.remove(*first);
    require(registry.find("live/generation").get() == second.get(), "registry stale remove preserves replacement");

    auto ended = std::make_shared<media_stream>("live/ended");
    ended->end();
    require(!registry.add(ended), "registry ended generation add reject");

    registry.remove(*second);
    require(!registry.find("live/generation"), "registry replacement removed");
}

void test_hls_output()
{
    hls_output output(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    output.on_track(make_video_track());
    output.on_track(make_audio_track());

    // 开始前的音频必须等待自然视频关键帧。
    output.on_frame(make_audio_frame(0));
    output.on_frame(make_video_frame(0, true));
    output.on_frame(make_audio_frame(20'000'000));
    output.on_frame(make_video_frame(500'000'000, false));
    output.on_frame(make_audio_frame(520'000'000));
    output.on_frame(make_video_frame(1'000'000'000, true));
    output.on_frame(make_audio_frame(1'020'000'000));
    output.on_frame(make_video_frame(1'500'000'000, false));
    output.on_frame(make_video_frame(2'000'000'000, true));
    output.on_end();

    require(output.segment_count() >= 2, "hls segment count");
    const auto first = output.segment(0);
    require(first.has_value() && !first->empty(), "first hls segment");
    require(first->size() % 188U == 0, "mpeg-ts packet alignment");
    require((*first)[0] == 0x47, "mpeg-ts sync byte");

    const auto playlist = output.playlist(".");
    require(playlist.find("#EXTM3U") != std::string::npos, "hls playlist header");
    require(playlist.find("#EXT-X-ENDLIST") != std::string::npos, "hls endlist");

    hls_output reconfigured(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    reconfigured.on_track(make_video_track());
    reconfigured.on_frame(make_video_frame(0, true));
    reconfigured.on_frame(make_video_frame(500'000'000, false));
    reconfigured.on_track(make_video_track(2));
    require(reconfigured.segment_count() == 1U, "hls config change closes current segment");
    reconfigured.on_frame(make_video_frame(600'000'000, false));
    require(reconfigured.segment_count() == 1U, "hls config change waits key frame");
    reconfigured.on_frame(make_video_frame(1'000'000'000, true));
    reconfigured.on_end();
    require(reconfigured.segment_count() == 2U, "hls config change starts new segment");

    hls_output audio_only(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    audio_only.on_track(make_audio_track());
    audio_only.on_frame(make_audio_frame(0));
    audio_only.on_frame(make_audio_frame(500'000'000));
    require(audio_only.segment_count() == 0U, "hls audio waits target duration");
    audio_only.on_frame(make_audio_frame(1'000'000'000));
    require(audio_only.segment_count() == 1U, "hls audio target duration segment");
    audio_only.on_end();
    require(audio_only.segment_count() == 2U, "hls audio final segment");
    const auto audio_segment = audio_only.segment(0);
    require(audio_segment.has_value() && !audio_segment->empty(), "hls audio segment data");
    require(audio_segment->size() % 188U == 0U, "hls audio mpeg-ts alignment");

    hls_output signed_timeline(hls_config{.target_duration_seconds = 1.0, .window_size = 4});
    signed_timeline.on_track(make_audio_track());
    signed_timeline.on_frame(make_audio_frame(-500'000'000));
    signed_timeline.on_frame(make_audio_frame(500'000'000));
    require(signed_timeline.segment_count() == 1U, "hls signed pts reaches target duration");
    signed_timeline.on_end();

    std::size_t flv_end_count = 0;
    const std::vector<media_track> flv_tracks{make_video_track()};
    http_flv_output flv_output(
        flv_tracks,
        [](std::span<const std::uint8_t>) {},
        [&flv_end_count]() { ++flv_end_count; });
    flv_output.on_track(make_video_track());
    flv_output.on_track(make_video_track(2));
    require(flv_end_count == 0U, "http flv existing track config update");
    flv_output.on_track(make_audio_track());
    flv_output.on_track(make_audio_track());
    require(flv_end_count == 1U, "http flv topology change closes once");
}


void test_hls_service_lifecycle()
{
    stream_registry registry;
    hls_service hls(registry, hls_config{.target_duration_seconds = 1.0, .window_size = 4});

    auto first = std::make_shared<media_stream>("live/hls");
    require(registry.add(first), "hls first stream add");
    require(first->update_track(make_video_track()), "hls first track");
    require(hls.segment_count("live/hls") == 0U, "hls first output create");

    require(first->publish(make_video_frame(0, true)), "hls first key frame");
    require(first->publish(make_video_frame(1'000'000'000, true)), "hls first segment boundary");
    first->end();
    registry.remove(*first);

    const auto ended_playlist = hls.playlist("live/hls");
    require(ended_playlist.has_value(), "hls ended playlist retained");
    require(ended_playlist->find("#EXT-X-ENDLIST") != std::string::npos, "hls ended playlist marker");
    const auto ended_segment = hls.segment("live/hls", 0);
    require(ended_segment.has_value() && !ended_segment->empty(), "hls ended segment retained");

    auto second = std::make_shared<media_stream>("live/hls");
    require(registry.add(second), "hls replacement stream add");
    require(second->update_track(make_video_track()), "hls replacement track");
    require(hls.segment_count("live/hls") == 0U, "hls replacement output reset");
    require(!hls.segment("live/hls", 0).has_value(), "hls replacement drops old segment");

    stream_registry expiring_registry;
    hls_service expiring_hls(
        expiring_registry,
        hls_config{.target_duration_seconds = 0.001, .window_size = 1});
    auto expiring_stream = std::make_shared<media_stream>("live/expiring");
    require(expiring_registry.add(expiring_stream), "hls expiring stream add");
    require(expiring_stream->update_track(make_video_track()), "hls expiring track");
    require(expiring_hls.segment_count("live/expiring") == 0U, "hls expiring output create");
    expiring_stream->end();
    expiring_registry.remove(*expiring_stream);
    require(expiring_hls.playlist("live/expiring").has_value(), "hls ended output initially retained");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!expiring_hls.playlist("live/expiring").has_value(), "hls ended output expires");
}


std::uint32_t rtp_timestamp(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp timestamp packet size");
    return (static_cast<std::uint32_t>(packet[4]) << 24U) |
        (static_cast<std::uint32_t>(packet[5]) << 16U) |
        (static_cast<std::uint32_t>(packet[6]) << 8U) |
        static_cast<std::uint32_t>(packet[7]);
}

std::uint16_t network_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 2U <= data.size(), "network u16 range");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8U) |
        static_cast<std::uint16_t>(data[offset + 1U]));
}

std::uint32_t network_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    require(offset + 4U <= data.size(), "network u32 range");
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(data[offset + 3U]);
}

std::uint32_t rtp_ssrc(const std::vector<std::uint8_t>& packet)
{
    require(packet.size() >= 12U, "rtp ssrc packet size");
    return network_u32(packet, 8U);
}

std::uint32_t require_rtcp_sender_report(
    const std::vector<std::uint8_t>& packet,
    std::string_view expected_cname)
{
    require(packet.size() >= 40U, "rtcp compound size");
    require((packet[0] >> 6U) == 2U, "rtcp sr version");
    require(packet[1] == 200U, "rtcp sender report type");

    const auto sr_size = (static_cast<std::size_t>(network_u16(packet, 2U)) + 1U) * 4U;
    require(sr_size >= 28U && sr_size + 12U <= packet.size(), "rtcp sender report size");
    const auto sender_ssrc = network_u32(packet, 4U);
    require(network_u32(packet, 20U) > 0U, "rtcp sender packet count");
    require(network_u32(packet, 24U) > 0U, "rtcp sender octet count");

    require((packet[sr_size] >> 6U) == 2U, "rtcp sdes version");
    require(packet[sr_size + 1U] == 202U, "rtcp sdes type");
    const auto sdes_size = (static_cast<std::size_t>(network_u16(packet, sr_size + 2U)) + 1U) * 4U;
    require(sr_size + sdes_size == packet.size(), "rtcp compound boundary");
    require(network_u32(packet, sr_size + 4U) == sender_ssrc, "rtcp sdes sender ssrc");
    require(packet[sr_size + 8U] == 1U, "rtcp sdes cname type");

    const auto cname_size = static_cast<std::size_t>(packet[sr_size + 9U]);
    require(sr_size + 10U + cname_size <= packet.size(), "rtcp cname range");
    const auto cname = std::string_view(
        reinterpret_cast<const char*>(packet.data() + sr_size + 10U),
        cname_size);
    require(cname == expected_cname, "rtcp sdes cname");
    return sender_ssrc;
}

void test_webrtc_rtp_packetizer()
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(webrtc_output_config{.video_payload_type = 102, .rtcp_cname = {}}, [&packets](std::span<const std::uint8_t> packet) {
        packets.emplace_back(packet.begin(), packet.end());
    });
    output.on_track(make_video_track());
    require(output.valid(), "webrtc video output valid");
    output.on_frame(make_video_frame(-40'000'000, false));
    require(packets.empty(), "webrtc waits natural key frame");
    output.on_frame(make_video_frame(0, true));

    require(!packets.empty() && packets.front().size() >= 12, "rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "rtp version 2");
    require((packets.front()[1] & 0x7fU) == 102U, "rtp negotiated payload type");

    const auto first_timestamp = rtp_timestamp(packets.front());
    const auto first_frame_packet_count = packets.size();
    output.on_frame(make_video_frame(40'000'000, false));
    require(packets.size() > first_frame_packet_count, "webrtc second h264 frame packetized");
    require(rtp_timestamp(packets.back()) - first_timestamp == 3'600U, "h264 rtp timestamp step");
}

void test_webrtc_opus_channel_count(int channel_count)
{
    std::vector<std::vector<std::uint8_t>> packets;
    webrtc_output output(
        webrtc_output_config{
            .opus_payload_type = 111,
            .opus_channel_count = channel_count,
            .rtcp_cname = {},
        },
        [&packets](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
        });
    output.on_track(make_audio_track());
    require(output.valid(), "webrtc audio output valid");

    std::int64_t pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = pts_ns,
            .pts_ns = pts_ns,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        pts_ns += 23'219'954;
    }

    require(!packets.empty() && packets.front().size() >= 12, "opus rtp header size");
    require((packets.front()[0] >> 6U) == 2U, "opus rtp version 2");
    require((packets.front()[1] & 0x7fU) == 111U, "opus negotiated payload type");

    if (packets.size() >= 2U)
    {
        require(rtp_timestamp(packets[1]) - rtp_timestamp(packets[0]) == 960U, "opus rtp timestamp step");
    }
}

void test_webrtc_output_initialization_failure()
{
    webrtc_output invalid_video(
        webrtc_output_config{.video_payload_type = 102, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    auto video = make_video_track();
    video.codec_config.clear();
    invalid_video.on_track(video);
    require(!invalid_video.valid(), "webrtc invalid h264 output rejected");

    webrtc_output invalid_audio(
        webrtc_output_config{.opus_payload_type = 111, .opus_channel_count = 3, .rtcp_cname = {}},
        [](std::span<const std::uint8_t>) {});
    invalid_audio.on_track(make_audio_track());
    require(!invalid_audio.valid(), "webrtc invalid opus output rejected");
}

void test_webrtc_rtcp_sender()
{
    constexpr std::string_view cname = "webrtc-test-cname";
    std::vector<std::vector<std::uint8_t>> rtp_packets;
    std::vector<std::vector<std::uint8_t>> rtcp_packets;
    webrtc_output output(
        webrtc_output_config{
            .video_payload_type = 102,
            .opus_payload_type = 111,
            .opus_channel_count = 2,
            .rtcp_cname = std::string(cname),
        },
        [&rtp_packets](std::span<const std::uint8_t> packet) {
            rtp_packets.emplace_back(packet.begin(), packet.end());
        },
        [&rtcp_packets](std::span<const std::uint8_t> packet) {
            rtcp_packets.emplace_back(packet.begin(), packet.end());
        });

    output.on_track(make_video_track());
    output.on_track(make_audio_track());
    output.on_frame(make_video_frame(0, true));

    std::int64_t audio_pts_ns = 0;
    for (const auto& adts : valid_aac_adts_frames)
    {
        output.on_frame(media_frame{
            .track = audio_track_id,
            .dts_ns = audio_pts_ns,
            .pts_ns = audio_pts_ns,
            .key_frame = false,
            .payload = std::make_shared<const std::vector<std::uint8_t>>(adts),
        });
        audio_pts_ns += 23'219'954;
    }

    std::optional<std::uint32_t> video_ssrc;
    std::optional<std::uint32_t> audio_ssrc;
    for (const auto& packet : rtp_packets)
    {
        require(packet.size() >= 12U, "rtcp sender rtp header");
        const auto payload_type = static_cast<std::uint8_t>(packet[1] & 0x7fU);
        if (payload_type == 102U)
        {
            video_ssrc = rtp_ssrc(packet);
        }
        else if (payload_type == 111U)
        {
            audio_ssrc = rtp_ssrc(packet);
        }
    }
    require(video_ssrc.has_value(), "rtcp sender video ssrc");
    require(audio_ssrc.has_value(), "rtcp sender audio ssrc");
    require(*video_ssrc != *audio_ssrc, "rtcp sender independent ssrc");

    bool video_report = false;
    bool audio_report = false;
    for (const auto& packet : rtcp_packets)
    {
        const auto sender_ssrc = require_rtcp_sender_report(packet, cname);
        video_report = video_report || sender_ssrc == *video_ssrc;
        audio_report = audio_report || sender_ssrc == *audio_ssrc;
    }
    require(video_report, "rtcp video sender report");
    require(audio_report, "rtcp audio sender report");
}

void test_webrtc_opus_packetizer()
{
    test_webrtc_opus_channel_count(1);
    test_webrtc_opus_channel_count(2);
}

}    // namespace
}    // namespace media_server

int main()
{
    using namespace media_server;
    test_timebase_conversions();
    std::cout << "[pass] timebase_conversions\n";
    test_rtmp_timestamp_timeline();
    std::cout << "[pass] rtmp_timestamp_timeline\n";
    test_internal_format_contract();
    std::cout << "[pass] internal_format_contract\n";
    test_flv_config_cache_lifecycle();
    std::cout << "[pass] flv_config_cache_lifecycle\n";
    test_h265_output_paths();
    std::cout << "[pass] h265_output_paths\n";
    test_rtsp_muxer_zero_origin_timeline();
    std::cout << "[pass] rtsp_muxer_zero_origin_timeline\n";
    test_rtsp_client_session_timeout();
    std::cout << "[pass] rtsp_client_session_timeout\n";
    test_rtsp_client_rejects_empty_media_selection();
    std::cout << "[pass] rtsp_client_rejects_empty_media_selection\n";
    test_rtsp_output_session_contract();
    std::cout << "[pass] rtsp_output_session_contract\n";
    test_rtsp_output_h265();
    std::cout << "[pass] rtsp_output_h265\n";
    test_rtsp_output_rejects_stale_description();
    std::cout << "[pass] rtsp_output_rejects_stale_description\n";
    test_media_stream_fanout_and_reentrancy();
    std::cout << "[pass] media_stream_fanout_and_reentrancy\n";
    test_stream_registry_generation_lifecycle();
    std::cout << "[pass] stream_registry_generation_lifecycle\n";
    test_hls_output();
    std::cout << "[pass] hls_output\n";
    test_hls_service_lifecycle();
    std::cout << "[pass] hls_service_lifecycle\n";
    test_webrtc_rtp_packetizer();
    std::cout << "[pass] webrtc_rtp_packetizer\n";
    test_webrtc_opus_packetizer();
    std::cout << "[pass] webrtc_opus_packetizer\n";
    test_webrtc_output_initialization_failure();
    std::cout << "[pass] webrtc_output_initialization_failure\n";
    test_webrtc_rtcp_sender();
    std::cout << "[pass] webrtc_rtcp_sender\n";
    std::cout << "all tests passed\n";
    return 0;
}
