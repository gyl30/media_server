#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

extern "C"
{
#include "rtsp-client.h"
#include "rtsp-header-transport.h"
}

namespace
{

using clock_type = std::chrono::steady_clock;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;

enum class transport_mode
{
    tcp_interleaved,
    udp_datagram,
};

struct endpoint
{
    std::string host;
    std::string port;
};

struct configuration
{
    endpoint signaling;
    std::string stream_prefix;
    std::string media_host{"127.0.0.1"};
    boost::asio::ip::address local_address{boost::asio::ip::address_v4::any()};
    std::uint16_t media_port{554};
    std::uint16_t udp_port_start{};
    std::size_t sources{};
    std::size_t ramp_per_second{100};
    std::size_t io_threads{4};
    std::uint32_t frames_per_second{25};
    std::uint64_t bitrate{710'000};
    std::chrono::seconds duration{15};
    transport_mode transport{transport_mode::tcp_interleaved};
};

struct allocation_result
{
    std::string stream_id;
    std::string publish_target;
};

struct process_sample
{
    double cpu_seconds{};
    std::uint64_t rss_kib{};
    std::uint64_t pss_kib{};
    std::size_t fds{};
};

struct shared_results
{
    std::atomic_size_t allocated{};
    std::atomic_size_t connected{};
    std::atomic_size_t ready{};
    std::atomic_size_t establishment_failures{};
    std::atomic_size_t runtime_failures{};
    std::atomic_size_t stopped{};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> errors;

    void add_error(std::string value)
    {
        std::lock_guard lock(mutex);
        if (errors.size() < 20U)
        {
            errors.push_back(std::move(value));
        }
    }

    void notify() { changed.notify_all(); }
};

struct media_fixture
{
    using packet_list = std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>;

    packet_list key_packets;
    packet_list delta_packets;
    std::uint64_t bitrate{};
};

std::string argument_value(int& index, int argc, char** argv, std::string_view name)
{
    if (index + 1 >= argc)
    {
        throw std::runtime_error("missing argument " + std::string(name));
    }
    return argv[++index];
}

endpoint parse_http_endpoint(std::string_view value)
{
    constexpr std::string_view prefix = "http://";
    if (!value.starts_with(prefix))
    {
        throw std::runtime_error("signaling URL must use http://");
    }
    value.remove_prefix(prefix.size());
    value = value.substr(0, value.find('/'));
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos)
    {
        return {std::string(value), "80"};
    }
    return {std::string(value.substr(0, colon)), std::string(value.substr(colon + 1U))};
}

configuration parse_arguments(int argc, char** argv)
{
    configuration config;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--signaling-url")
        {
            config.signaling = parse_http_endpoint(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--stream-prefix")
        {
            config.stream_prefix = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-host")
        {
            config.media_host = argument_value(index, argc, argv, argument);
        }
        else if (argument == "--media-port")
        {
            config.media_port = static_cast<std::uint16_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--local-address")
        {
            boost::system::error_code error;
            config.local_address = boost::asio::ip::make_address(argument_value(index, argc, argv, argument), error);
            if (error || !config.local_address.is_v4())
            {
                throw std::runtime_error("local-address must be an IPv4 address");
            }
        }
        else if (argument == "--sources")
        {
            config.sources = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--udp-port-start")
        {
            config.udp_port_start = static_cast<std::uint16_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--ramp-per-second")
        {
            config.ramp_per_second = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--io-threads")
        {
            config.io_threads = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--fps")
        {
            config.frames_per_second = static_cast<std::uint32_t>(std::stoul(argument_value(index, argc, argv, argument)));
        }
        else if (argument == "--bitrate")
        {
            config.bitrate = std::stoull(argument_value(index, argc, argv, argument));
        }
        else if (argument == "--duration")
        {
            config.duration = std::chrono::seconds{std::stoll(argument_value(index, argc, argv, argument))};
        }
        else if (argument == "--transport")
        {
            const auto value = argument_value(index, argc, argv, argument);
            if (value == "tcp")
            {
                config.transport = transport_mode::tcp_interleaved;
            }
            else if (value == "udp")
            {
                config.transport = transport_mode::udp_datagram;
            }
            else
            {
                throw std::runtime_error("transport must be tcp or udp");
            }
        }
        else
        {
            throw std::runtime_error("unknown argument " + std::string(argument));
        }
    }
    if (config.signaling.host.empty() || config.stream_prefix.empty() || config.sources == 0U || config.ramp_per_second == 0U ||
        config.io_threads == 0U || config.io_threads > 64U || config.frames_per_second == 0U || config.duration.count() <= 0)
    {
        throw std::runtime_error("signaling-url, stream-prefix, sources, ramp-per-second, io-threads, fps and positive duration are required");
    }
    if (config.udp_port_start != 0U && ((config.udp_port_start % 2U) != 0U || config.sources > (65'536U - config.udp_port_start) / 2U))
    {
        throw std::runtime_error("udp-port-start must be even and leave one port pair per source");
    }
    if (config.transport != transport_mode::udp_datagram && config.udp_port_start != 0U)
    {
        throw std::runtime_error("udp-port-start requires UDP transport");
    }
    const auto bytes_per_frame = config.bitrate / 8U / config.frames_per_second;
    if (bytes_per_frame <= 1'200U)
    {
        throw std::runtime_error("bitrate must provide more than 1200 bytes per frame");
    }
    return config;
}

std::string rtsp_target(std::string_view url)
{
    constexpr std::string_view prefix = "rtsp://";
    if (!url.starts_with(prefix))
    {
        throw std::runtime_error("publish URL must use rtsp://");
    }
    const auto slash = url.find('/', prefix.size());
    if (slash == std::string_view::npos)
    {
        throw std::runtime_error("publish URL has no path");
    }
    return std::string(url.substr(slash));
}

class allocation_client final
{
   public:
    explicit allocation_client(endpoint target) : target_(std::move(target)), resolver_(io_), stream_(io_) {}

    allocation_result allocate(std::string_view stream_name)
    {
        namespace http = boost::beast::http;
        connect();

        boost::json::object body;
        body["protocol"] = "rtsp";
        body["stream_name"] = stream_name;
        http::request<http::string_body> request{http::verb::post, "/api/publish/allocations", 11};
        request.set(http::field::host, target_.host);
        request.set(http::field::content_type, "application/json");
        request.keep_alive(true);
        request.body() = boost::json::serialize(body);
        request.prepare_payload();

        boost::system::error_code error;
        stream_.expires_after(std::chrono::seconds{5});
        http::write(stream_, request, error);
        if (error)
        {
            throw std::runtime_error("allocation write: " + error.message());
        }
        http::response<http::string_body> response;
        http::read(stream_, buffer_, response, error);
        if (error)
        {
            throw std::runtime_error("allocation read: " + error.message());
        }
        if (response.result() != http::status::created)
        {
            throw std::runtime_error("allocation HTTP status " + std::to_string(response.result_int()) + ": " + response.body());
        }
        const auto object = boost::json::parse(response.body()).as_object();
        const auto stream_id = object.at("stream_id").as_string();
        const auto publish_url = object.at("publish_url").as_string();
        if (!response.keep_alive())
        {
            close();
        }
        return {std::string(stream_id), rtsp_target(std::string_view(publish_url.data(), publish_url.size()))};
    }

   private:
    void connect()
    {
        if (stream_.socket().is_open())
        {
            return;
        }
        boost::system::error_code error;
        const auto endpoints = resolver_.resolve(target_.host, target_.port, error);
        if (error)
        {
            throw std::runtime_error("allocation resolve: " + error.message());
        }
        stream_.expires_after(std::chrono::seconds{5});
        stream_.connect(endpoints, error);
        if (error)
        {
            throw std::runtime_error("allocation connect: " + error.message());
        }
    }

    void close()
    {
        boost::system::error_code ignored;
        stream_.socket().close(ignored);
        buffer_.consume(buffer_.size());
    }

   private:
    endpoint target_;
    boost::asio::io_context io_;
    tcp::resolver resolver_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
};

media_fixture make_media_fixture(const configuration& config)
{
    constexpr std::size_t maximum_payload = 1'200U;
    constexpr std::array<std::uint8_t, 10> idr{0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0};
    constexpr std::array<std::uint8_t, 5> predicted{0x41, 0x9a, 0x20, 0x26, 0x94};
    const auto bytes_per_frame = static_cast<std::size_t>(config.bitrate / 8U / config.frames_per_second);
    media_fixture fixture;
    const auto filler_count = (bytes_per_frame - predicted.size() + maximum_payload - 1U) / maximum_payload;
    const auto make_frame = [bytes_per_frame, filler_count, maximum_payload](const auto& first_nalu)
    {
        media_fixture::packet_list packets;
        packets.reserve(filler_count + 1U);
        packets.emplace_back(std::make_shared<const std::vector<std::uint8_t>>(first_nalu.begin(), first_nalu.end()));
        auto remaining = bytes_per_frame - first_nalu.size();
        for (std::size_t index = 0; index < filler_count; ++index)
        {
            const auto packets_left = filler_count - index;
            const auto bytes = std::min(maximum_payload, remaining - (packets_left - 1U) * 2U);
            auto payload = std::make_shared<std::vector<std::uint8_t>>(bytes, 0xffU);
            (*payload)[0] = 0x0c;
            (*payload)[bytes - 1U] = 0x80;
            packets.emplace_back(std::move(payload));
            remaining -= bytes;
        }
        return packets;
    };
    fixture.key_packets = make_frame(idr);
    fixture.delta_packets = make_frame(predicted);
    fixture.bitrate = static_cast<std::uint64_t>(bytes_per_frame) * config.frames_per_second * 8U;
    return fixture;
}

std::uint64_t read_kib_value(const std::filesystem::path& path, std::string_view key)
{
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.starts_with(key))
        {
            std::istringstream value_stream(line.substr(key.size()));
            std::uint64_t value{};
            value_stream >> value;
            return value;
        }
    }
    return 0;
}

process_sample sample_process()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        throw std::runtime_error("getrusage failed");
    }
    const auto seconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1'000'000.0;
    std::size_t fds{};
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        (void)entry;
        ++fds;
    }
    return {
        seconds,
        read_kib_value("/proc/self/status", "VmRSS:"),
        read_kib_value("/proc/self/smaps_rollup", "Pss:"),
        fds,
    };
}

class publish_session final : public std::enable_shared_from_this<publish_session>
{
   public:
    publish_session(boost::asio::io_context& io,
                    const configuration& config,
                    std::shared_ptr<const media_fixture> fixture,
                    std::shared_ptr<shared_results> results,
                    std::size_t index,
                    std::string publish_target)
        : config_(config),
          fixture_(std::move(fixture)),
          results_(std::move(results)),
          index_(index),
          publish_target_(std::move(publish_target)),
          socket_(io),
          udp_rtp_(io),
          udp_rtcp_(io),
          deadline_(io),
          pacing_(io),
          ssrc_(0x4000'0000U + static_cast<std::uint32_t>(index)),
          sequence_(static_cast<std::uint16_t>(index * 97U)),
          timestamp_(static_cast<std::uint32_t>(index * 3'601U)),
          headers_(fixture_->key_packets.size()),
          tcp_buffers_(fixture_->key_packets.size() * 2U)
    {
    }

    ~publish_session()
    {
        if (client_ != nullptr)
        {
            rtsp_client_destroy(client_);
        }
    }

    void start(const tcp::endpoint& endpoint)
    {
        if (!config_.local_address.is_unspecified())
        {
            boost::system::error_code error;
            socket_.open(endpoint.protocol(), error);
            if (!error)
            {
                socket_.bind({config_.local_address, 0}, error);
            }
            if (error)
            {
                fail("control_bind", error);
                return;
            }
        }
        deadline_.expires_after(std::chrono::seconds{10});
        deadline_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& error)
            {
                if (!error)
                {
                    self->fail("handshake_timeout", boost::asio::error::timed_out);
                }
            });
        socket_.async_connect(endpoint,
                              [self = shared_from_this()](const boost::system::error_code& error)
                              {
                                  if (error)
                                  {
                                      self->fail("connect", error);
                                      return;
                                  }
                                  ++self->results_->connected;
                                  self->begin_rtsp();
                              });
    }

    void stop()
    {
        if (terminal_)
        {
            return;
        }
        intentional_stop_ = true;
        close();
    }

    [[nodiscard]] std::uint64_t sent_bytes() const noexcept { return sent_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t sent_packets() const noexcept { return sent_packets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t sent_frames() const noexcept { return sent_frames_.load(std::memory_order_relaxed); }

   private:
    void begin_rtsp()
    {
        if (config_.transport == transport_mode::udp_datagram)
        {
            boost::system::error_code error;
            if (!bind_udp_pair(error))
            {
                fail("udp_bind", error);
                return;
            }
            read_rtcp();
        }

        const auto authority = config_.media_host + ':' + std::to_string(config_.media_port);
        uri_ = "rtsp://" + authority + publish_target_;
        auto control_target = publish_target_.substr(0, publish_target_.find('?'));
        const auto control_uri = "rtsp://" + authority + control_target + "/trackID=0";
        const auto local_address = config_.local_address.is_unspecified() ? "127.0.0.1" : config_.local_address.to_string();
        sdp_ =
            "v=0\r\n"
            "o=- 0 0 IN IP4 " +
            local_address +
            "\r\n"
            "s=fanout-rtsp-publisher\r\n"
            "c=IN IP4 " +
            local_address +
            "\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=42c00a;"
            "sprop-parameter-sets=Z0LACtp7ARAAAAMAEAAAAwMo8SJq,aM4PyA==\r\n"
            "a=control:" +
            control_uri + "\r\n";

        rtsp_client_handler_t handler{};
        handler.send = &publish_session::send_callback;
        handler.rtpport = &publish_session::rtp_port_callback;
        handler.onannounce = &publish_session::announce_callback;
        handler.ondescribe = &publish_session::describe_callback;
        handler.onsetup = &publish_session::setup_callback;
        handler.onplay = &publish_session::play_callback;
        handler.onrecord = &publish_session::record_callback;
        handler.onpause = &publish_session::simple_callback;
        handler.onteardown = &publish_session::simple_callback;
        handler.onrtp = &publish_session::rtp_callback;
        client_ = rtsp_client_create(uri_.c_str(), nullptr, nullptr, &handler, this);
        if (client_ == nullptr)
        {
            fail("rtsp_create", boost::asio::error::operation_aborted);
            return;
        }
        read_control();
        if (rtsp_client_announce(client_, sdp_.c_str()) != 0)
        {
            fail("announce", boost::asio::error::operation_aborted);
        }
    }

    bool bind_udp_pair(boost::system::error_code& error)
    {
        if (config_.udp_port_start != 0U)
        {
            const auto rtp_port = static_cast<std::uint16_t>(config_.udp_port_start + index_ * 2U);
            udp_rtp_.open(udp::v4(), error);
            if (!error)
            {
                udp_rtp_.bind({config_.local_address, rtp_port}, error);
            }
            if (!error)
            {
                udp_rtcp_.open(udp::v4(), error);
            }
            if (!error)
            {
                udp_rtcp_.bind({config_.local_address, static_cast<std::uint16_t>(rtp_port + 1U)}, error);
            }
            if (!error)
            {
                return true;
            }
            boost::system::error_code ignored;
            udp_rtp_.close(ignored);
            udp_rtcp_.close(ignored);
            return false;
        }
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            udp_rtp_.open(udp::v4(), error);
            if (error)
            {
                return false;
            }
            udp_rtp_.bind({config_.local_address, 0}, error);
            if (error)
            {
                udp_rtp_.close();
                continue;
            }
            const auto rtp_port = udp_rtp_.local_endpoint(error).port();
            if (error || (rtp_port % 2U) != 0U || rtp_port == 65'535U)
            {
                udp_rtp_.close();
                error.clear();
                continue;
            }
            udp_rtcp_.open(udp::v4(), error);
            if (!error)
            {
                udp_rtcp_.bind({config_.local_address, static_cast<std::uint16_t>(rtp_port + 1U)}, error);
            }
            if (!error)
            {
                return true;
            }
            boost::system::error_code ignored;
            udp_rtp_.close(ignored);
            udp_rtcp_.close(ignored);
            error.clear();
        }
        error = boost::asio::error::address_in_use;
        return false;
    }

    void read_control()
    {
        socket_.async_read_some(boost::asio::buffer(control_read_),
                                [self = shared_from_this()](const boost::system::error_code& error, std::size_t bytes)
                                {
                                    if (error)
                                    {
                                        if (!self->intentional_stop_)
                                        {
                                            self->fail("control_read", error);
                                        }
                                        return;
                                    }
                                    if (rtsp_client_input(self->client_, self->control_read_.data(), bytes) != 0)
                                    {
                                        self->fail("rtsp_input", boost::asio::error::operation_aborted);
                                        return;
                                    }
                                    if (!self->terminal_)
                                    {
                                        self->read_control();
                                    }
                                });
    }

    void read_rtcp()
    {
        udp_rtcp_.async_receive(boost::asio::buffer(rtcp_read_),
                                [self = shared_from_this()](const boost::system::error_code& error, std::size_t)
                                {
                                    if (!error && !self->terminal_)
                                    {
                                        self->read_rtcp();
                                    }
                                });
    }

    void enqueue_control(const void* data, std::size_t bytes)
    {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        control_writes_.emplace_back(begin, begin + bytes);
        if (!control_write_active_)
        {
            write_control();
        }
    }

    void write_control()
    {
        if (control_writes_.empty() || terminal_)
        {
            control_write_active_ = false;
            return;
        }
        control_write_active_ = true;
        boost::asio::async_write(socket_,
                                 boost::asio::buffer(control_writes_.front()),
                                 [self = shared_from_this()](const boost::system::error_code& error, std::size_t)
                                 {
                                     if (error)
                                     {
                                         self->fail("control_write", error);
                                         return;
                                     }
                                     self->control_writes_.pop_front();
                                     self->write_control();
                                 });
    }

    void start_media()
    {
        if (ready_ || terminal_)
        {
            return;
        }
        ready_ = true;
        deadline_.cancel();
        ++results_->ready;
        results_->notify();
        next_frame_ = clock_type::now();
        send_frame();
    }

    void send_frame()
    {
        if (terminal_)
        {
            return;
        }
        current_packets_ = frame_index_ % config_.frames_per_second == 0U ? &fixture_->key_packets : &fixture_->delta_packets;
        prepare_headers();
        if (config_.transport == transport_mode::tcp_interleaved)
        {
            for (std::size_t index = 0; index < current_packets_->size(); ++index)
            {
                tcp_buffers_[index * 2U] = boost::asio::buffer(headers_[index]);
                tcp_buffers_[index * 2U + 1U] = boost::asio::buffer(*(*current_packets_)[index]);
            }
            boost::asio::async_write(socket_,
                                     tcp_buffers_,
                                     [self = shared_from_this()](const boost::system::error_code& error, std::size_t bytes)
                                     {
                                         if (error)
                                         {
                                             self->fail("media_write", error);
                                             return;
                                         }
                                         self->sent_bytes_.fetch_add(bytes, std::memory_order_relaxed);
                                         self->sent_packets_.fetch_add(self->current_packets_->size(), std::memory_order_relaxed);
                                         self->frame_complete();
                                     });
        }
        else
        {
            udp_fragment_ = 0U;
            send_udp_fragment();
        }
    }

    void send_udp_fragment()
    {
        const auto index = udp_fragment_;
        std::array<boost::asio::const_buffer, 2> buffers{
            boost::asio::buffer(headers_[index].data() + 4U, headers_[index].size() - 4U),
            boost::asio::buffer(*(*current_packets_)[index]),
        };
        udp_rtp_.async_send_to(buffers,
                               udp_rtp_target_,
                               [self = shared_from_this()](const boost::system::error_code& error, std::size_t bytes)
                               {
                                   if (error)
                                   {
                                       self->fail("media_send", error);
                                       return;
                                   }
                                   self->sent_bytes_.fetch_add(bytes, std::memory_order_relaxed);
                                   self->sent_packets_.fetch_add(1U, std::memory_order_relaxed);
                                   if (++self->udp_fragment_ < self->current_packets_->size())
                                   {
                                       self->send_udp_fragment();
                                   }
                                   else
                                   {
                                       self->frame_complete();
                                   }
                               });
    }

    void prepare_headers()
    {
        for (std::size_t index = 0; index < headers_.size(); ++index)
        {
            auto& header = headers_[index];
            const auto payload_bytes = static_cast<std::uint16_t>(12U + (*current_packets_)[index]->size());
            header[0] = 0x24;
            header[1] = rtp_channel_;
            header[2] = static_cast<std::uint8_t>(payload_bytes >> 8U);
            header[3] = static_cast<std::uint8_t>(payload_bytes);
            header[4] = 0x80;
            header[5] = static_cast<std::uint8_t>((index + 1U == headers_.size() ? 0x80U : 0U) | 96U);
            header[6] = static_cast<std::uint8_t>(sequence_ >> 8U);
            header[7] = static_cast<std::uint8_t>(sequence_);
            ++sequence_;
            header[8] = static_cast<std::uint8_t>(timestamp_ >> 24U);
            header[9] = static_cast<std::uint8_t>(timestamp_ >> 16U);
            header[10] = static_cast<std::uint8_t>(timestamp_ >> 8U);
            header[11] = static_cast<std::uint8_t>(timestamp_);
            header[12] = static_cast<std::uint8_t>(ssrc_ >> 24U);
            header[13] = static_cast<std::uint8_t>(ssrc_ >> 16U);
            header[14] = static_cast<std::uint8_t>(ssrc_ >> 8U);
            header[15] = static_cast<std::uint8_t>(ssrc_);
        }
    }

    void frame_complete()
    {
        sent_frames_.fetch_add(1U, std::memory_order_relaxed);
        ++frame_index_;
        const auto interval = std::chrono::nanoseconds{1'000'000'000LL / config_.frames_per_second};
        const auto timestamp_step = 90'000U / config_.frames_per_second;
        timestamp_ += timestamp_step;
        next_frame_ += interval;
        const auto now = clock_type::now();
        if (next_frame_ <= now)
        {
            const auto missed = static_cast<std::uint32_t>((now - next_frame_) / interval + 1U);
            next_frame_ += interval * missed;
            timestamp_ += timestamp_step * missed;
            frame_index_ += missed;
        }
        pacing_.expires_at(next_frame_);
        pacing_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& error)
            {
                if (!error)
                {
                    self->send_frame();
                }
            });
    }

    void fail(std::string_view phase, const boost::system::error_code& error)
    {
        if (terminal_ || intentional_stop_)
        {
            return;
        }
        if (ready_)
        {
            ++results_->runtime_failures;
        }
        else
        {
            ++results_->establishment_failures;
        }
        results_->add_error("source=" + std::to_string(index_) + " phase=" + std::string(phase) + " error=" + std::to_string(error.value()) + ':' +
                            error.message());
        results_->notify();
        close();
    }

    void close()
    {
        if (terminal_)
        {
            return;
        }
        terminal_ = true;
        boost::system::error_code ignored;
        deadline_.cancel();
        pacing_.cancel();
        socket_.close(ignored);
        udp_rtp_.close(ignored);
        udp_rtcp_.close(ignored);
        ++results_->stopped;
        results_->notify();
    }

    static int send_callback(void* param, const char*, const void* request, std::size_t bytes)
    {
        auto* self = static_cast<publish_session*>(param);
        self->enqueue_control(request, bytes);
        return static_cast<int>(bytes);
    }

    static int rtp_port_callback(void* param, int, const char*, unsigned short port[2], char* ip, int length)
    {
        auto* self = static_cast<publish_session*>(param);
        if (self->config_.transport == transport_mode::tcp_interleaved)
        {
            port[0] = 0;
            port[1] = 1;
            return RTSP_TRANSPORT_RTP_TCP;
        }
        boost::system::error_code error;
        const auto address = self->socket_.local_endpoint(error).address().to_string();
        if (error || std::snprintf(ip, static_cast<std::size_t>(length), "%s", address.c_str()) >= length)
        {
            return -1;
        }
        port[0] = self->udp_rtp_.local_endpoint(error).port();
        if (error)
        {
            return -1;
        }
        port[1] = self->udp_rtcp_.local_endpoint(error).port();
        return error ? -1 : RTSP_TRANSPORT_RTP_UDP;
    }

    static int announce_callback(void* param)
    {
        auto* self = static_cast<publish_session*>(param);
        return rtsp_client_setup(self->client_, self->sdp_.c_str(), static_cast<int>(self->sdp_.size()));
    }

    static int describe_callback(void*, const char*, int) { return -1; }

    static int setup_callback(void* param, int, std::int64_t)
    {
        auto* self = static_cast<publish_session*>(param);
        const auto* transport = rtsp_client_get_media_transport(self->client_, 0);
        if (transport == nullptr)
        {
            return -1;
        }
        if (self->config_.transport == transport_mode::tcp_interleaved)
        {
            self->rtp_channel_ = static_cast<std::uint8_t>(transport->interleaved1);
        }
        else
        {
            boost::system::error_code error;
            const auto address = transport->source[0] != '\0' ? boost::asio::ip::make_address(transport->source, error)
                                                              : self->socket_.remote_endpoint(error).address();
            if (error || transport->rtp.u.server_port1 == 0U)
            {
                return -1;
            }
            self->udp_rtp_target_ = {address, transport->rtp.u.server_port1};
        }
        return rtsp_client_record(self->client_, nullptr, nullptr);
    }

    static int play_callback(void*, int, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t*, int) { return -1; }

    static int record_callback(void* param, int, const std::uint64_t*, const std::uint64_t*, const double*, const rtsp_rtp_info_t*, int)
    {
        static_cast<publish_session*>(param)->start_media();
        return 0;
    }

    static int simple_callback(void*) { return 0; }
    static void rtp_callback(void*, std::uint8_t, const void*, std::uint16_t) {}

   private:
    const configuration& config_;
    std::shared_ptr<const media_fixture> fixture_;
    std::shared_ptr<shared_results> results_;
    std::size_t index_{};
    std::string publish_target_;
    std::string uri_;
    std::string sdp_;
    tcp::socket socket_;
    udp::socket udp_rtp_;
    udp::socket udp_rtcp_;
    udp::endpoint udp_rtp_target_;
    boost::asio::steady_timer deadline_;
    boost::asio::steady_timer pacing_;
    std::array<std::uint8_t, 8'192> control_read_{};
    std::array<std::uint8_t, 1'500> rtcp_read_{};
    std::deque<std::vector<std::uint8_t>> control_writes_;
    rtsp_client_t* client_{};
    std::uint32_t ssrc_{};
    std::uint16_t sequence_{};
    std::uint32_t timestamp_{};
    std::uint8_t rtp_channel_{};
    std::vector<std::array<std::uint8_t, 16>> headers_;
    std::vector<boost::asio::const_buffer> tcp_buffers_;
    const media_fixture::packet_list* current_packets_{};
    std::size_t udp_fragment_{};
    std::uint64_t frame_index_{};
    clock_type::time_point next_frame_{};
    std::atomic_uint64_t sent_bytes_{};
    std::atomic_uint64_t sent_packets_{};
    std::atomic_uint64_t sent_frames_{};
    bool control_write_active_{};
    bool ready_{};
    bool intentional_stop_{};
    bool terminal_{};
};

struct io_shard
{
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::thread thread;

    ~io_shard()
    {
        work.reset();
        io.stop();
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

std::pair<std::uint64_t, std::uint64_t> media_totals(const std::vector<std::shared_ptr<publish_session>>& sessions)
{
    std::uint64_t bytes{};
    std::uint64_t packets{};
    for (const auto& session : sessions)
    {
        bytes += session->sent_bytes();
        packets += session->sent_packets();
    }
    return {bytes, packets};
}

std::size_t progressing_sessions(const std::vector<std::shared_ptr<publish_session>>& sessions, const std::vector<std::uint64_t>& before)
{
    std::size_t progressing{};
    for (std::size_t index = 0; index < sessions.size(); ++index)
    {
        if (sessions[index]->sent_frames() > before[index])
        {
            ++progressing;
        }
    }
    return progressing;
}

void stop_shards(std::vector<std::unique_ptr<io_shard>>& shards)
{
    for (auto& shard : shards)
    {
        shard->work.reset();
        shard->io.stop();
    }
    for (auto& shard : shards)
    {
        if (shard->thread.joinable())
        {
            shard->thread.join();
        }
    }
}

}    // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto config = parse_arguments(argc, argv);
        const auto media_address = boost::asio::ip::make_address(config.media_host);
        const tcp::endpoint media_endpoint{media_address, config.media_port};
        auto fixture = std::make_shared<const media_fixture>(make_media_fixture(config));
        auto results = std::make_shared<shared_results>();
        allocation_client allocations(config.signaling);

        std::vector<std::unique_ptr<io_shard>> shards;
        shards.reserve(config.io_threads);
        for (std::size_t index = 0; index < config.io_threads; ++index)
        {
            auto shard = std::make_unique<io_shard>();
            shard->thread = std::thread([pointer = shard.get()]() { pointer->io.run(); });
            shards.emplace_back(std::move(shard));
        }

        std::vector<std::shared_ptr<publish_session>> sessions;
        sessions.reserve(config.sources);
        const auto ramp_started = clock_type::now();
        const auto ramp_interval = std::chrono::nanoseconds{1'000'000'000LL / static_cast<std::int64_t>(config.ramp_per_second)};
        std::string first_stream_id;
        std::string first_stream_name;
        for (std::size_t index = 0; index < config.sources; ++index)
        {
            const auto stream_name = config.stream_prefix + '/' + std::to_string(index);
            const auto allocation = allocations.allocate(stream_name);
            ++results->allocated;
            if (index == 0U)
            {
                first_stream_id = allocation.stream_id;
                first_stream_name = stream_name;
            }
            auto& shard = *shards[index % shards.size()];
            auto session = std::make_shared<publish_session>(shard.io, config, fixture, results, index, allocation.publish_target);
            sessions.emplace_back(session);
            boost::asio::post(shard.io, [session, media_endpoint]() { session->start(media_endpoint); });
            const auto next = ramp_started + ramp_interval * static_cast<std::int64_t>(index + 1U);
            if (next > clock_type::now())
            {
                std::this_thread::sleep_until(next);
            }
        }

        const auto establishment_deadline = clock_type::now() + std::chrono::seconds{30};
        {
            std::unique_lock lock(results->mutex);
            results->changed.wait_until(
                lock, establishment_deadline, [&]() { return results->ready.load() + results->establishment_failures.load() == config.sources; });
        }
        const auto ramp_elapsed = std::chrono::duration<double>(clock_type::now() - ramp_started).count();
        const auto established_sample = sample_process();
        std::cout << "phase=established requested=" << config.sources << " allocated=" << results->allocated.load()
                  << " connected=" << results->connected.load() << " ready=" << results->ready.load()
                  << " establishment_failures=" << results->establishment_failures.load() << " ramp_seconds=" << ramp_elapsed
                  << " generator_cpu_seconds=" << established_sample.cpu_seconds << " generator_rss_kib=" << established_sample.rss_kib
                  << " generator_pss_kib=" << established_sample.pss_kib << " generator_fds=" << established_sample.fds << '\n';
        std::cout << "transport=" << (config.transport == transport_mode::tcp_interleaved ? "tcp" : "udp") << " io_threads=" << config.io_threads
                  << " fps=" << config.frames_per_second << " requested_bitrate=" << config.bitrate
                  << " local_address=" << config.local_address.to_string() << " udp_port_start=" << config.udp_port_start
                  << " actual_fixture_bitrate=" << fixture->bitrate << " packets_per_frame=" << fixture->key_packets.size()
                  << " first_stream_name=" << first_stream_name << " first_stream_id=" << first_stream_id << '\n';

        if (results->ready.load() != config.sources)
        {
            for (const auto& error : results->errors)
            {
                std::cerr << error << '\n';
            }
            for (std::size_t index = 0; index < sessions.size(); ++index)
            {
                const auto session = sessions[index];
                boost::asio::post(shards[index % shards.size()]->io, [session]() { session->stop(); });
            }
            stop_shards(shards);
            return 1;
        }

        std::vector<std::uint64_t> frames_before;
        frames_before.reserve(sessions.size());
        for (const auto& session : sessions)
        {
            frames_before.push_back(session->sent_frames());
        }
        const auto [bytes_before, packets_before] = media_totals(sessions);
        const auto measurement_process_before = sample_process();
        const auto measurement_started = clock_type::now();
        std::this_thread::sleep_for(config.duration);
        const auto measurement_elapsed = std::chrono::duration<double>(clock_type::now() - measurement_started).count();
        const auto measurement_process_after = sample_process();
        const auto [bytes_after, packets_after] = media_totals(sessions);
        const auto progressing = progressing_sessions(sessions, frames_before);
        const auto sent_bytes = bytes_after - bytes_before;
        const auto sent_packets = packets_after - packets_before;
        std::cout << "phase=measurement progressing=" << progressing << " runtime_failures=" << results->runtime_failures.load()
                  << " duration_seconds=" << measurement_elapsed << " sent_bytes=" << sent_bytes << " sent_packets=" << sent_packets
                  << " aggregate_bytes_per_second=" << static_cast<double>(sent_bytes) / measurement_elapsed
                  << " bytes_per_source_second=" << static_cast<double>(sent_bytes) / measurement_elapsed / static_cast<double>(config.sources)
                  << " generator_cpu_cores=" << (measurement_process_after.cpu_seconds - measurement_process_before.cpu_seconds) / measurement_elapsed
                  << " generator_rss_kib=" << measurement_process_after.rss_kib << " generator_pss_kib=" << measurement_process_after.pss_kib
                  << " generator_fds=" << measurement_process_after.fds << '\n';

        for (std::size_t index = 0; index < sessions.size(); ++index)
        {
            const auto session = sessions[index];
            boost::asio::post(shards[index % shards.size()]->io, [session]() { session->stop(); });
        }
        {
            std::unique_lock lock(results->mutex);
            results->changed.wait_for(lock, std::chrono::seconds{5}, [&]() { return results->stopped.load() == config.sources; });
        }
        std::cout << "phase=disconnect stopped=" << results->stopped.load() << " requested=" << config.sources << '\n';
        for (const auto& error : results->errors)
        {
            std::cerr << error << '\n';
        }
        stop_shards(shards);
        return progressing == config.sources && results->runtime_failures.load() == 0U && results->stopped.load() == config.sources ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fanout RTSP publisher failed: " << error.what() << '\n';
        return 2;
    }
}
