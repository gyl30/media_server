#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/net/media_port_pool.h"
#include "media/core/session_registry.h"
#include "media/gb28181/gb28181_playback_session.h"

namespace media_server
{
namespace
{
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
using error_code = boost::system::error_code;
constexpr auto operation_limit = 2s;
constexpr std::uint32_t test_ssrc = 0x12345678U;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T>
T get_ready(std::future<T>& future)
{
    require(future.wait_for(operation_limit) == std::future_status::ready, "worker barrier timed out");
    return future.get();
}

template <typename Function>
void on(worker_context& worker, Function function)
{
    auto result = std::make_shared<std::promise<void>>();
    auto future = result->get_future();
    boost::asio::post(worker.io(),
                      [result, function = std::move(function)]() mutable
                      {
                          try
                          {
                              function();
                              result->set_value();
                          }
                          catch (...)
                          {
                              result->set_exception(std::current_exception());
                          }
                      });
    get_ready(future);
}

// 不轮询或固定休眠：本函数返回前必须收到实际操作或取消的完成回调。
template <typename Begin>
error_code peer_operation(boost::asio::io_context& io,
                          tcp::socket& socket,
                          Begin begin,
                          std::chrono::steady_clock::duration timeout = operation_limit)
{
    io.restart();
    boost::asio::steady_timer timer(io, timeout);
    bool complete = false;
    bool timed_out = false;
    error_code result;
    timer.async_wait(
        [&](error_code error)
        {
            if (!error)
            {
                timed_out = true;
                error_code ignored;
                socket.close(ignored);
            }
        });
    begin(
        [&](error_code error, std::size_t)
        {
            result = error;
            complete = true;
            timer.cancel();
        });
    io.run();
    require(complete && !timed_out, "peer operation timed out");
    return result;
}

class fixture
{
   public:
    fixture()
        : id("playback-lifecycle-" + std::to_string(next_id.fetch_add(1))),
          receiver(std::make_shared<gb28181_playback_session>(owner, id, 96, test_ssrc)),
          weak(receiver),
          source_thread([this]() { source.run(); }),
          owner_thread([this]() { owner.run(); })
    {
        try
        {
            on(owner,
               [this]()
               {
                   require(session_registry::instance().add_receiver_session(id, receiver), "register receiver");
                   const auto port = receiver->startup(boost::asio::ip::address_v4::loopback());
                   require(port.has_value(), "listen on allocated port");
                   endpoint = {boost::asio::ip::address_v4::loopback(), *port};
               });
        }
        catch (...)
        {
            cleanup();
            throw;
        }
    }

    ~fixture() { cleanup(); }

    std::unique_ptr<tcp::socket> connect(tcp::endpoint destination, int receive_buffer_size = 0)
    {
        auto socket = std::make_unique<tcp::socket>(peer_io);
        if (receive_buffer_size != 0)
        {
            error_code error;
            socket->open(destination.protocol(), error);
            require(!error, "open peer before receive buffer setup");
            socket->set_option(boost::asio::socket_base::receive_buffer_size(receive_buffer_size), error);
            require(!error, "peer receive buffer before connect");
        }
        const auto error =
            peer_operation(peer_io, *socket, [&](auto done) { socket->async_connect(destination, [done](error_code ec) { done(ec, 0); }); });
        require(!error, "peer connect");
        return socket;
    }

    std::unique_ptr<tcp::socket> device() { return connect(endpoint); }

    std::unique_ptr<tcp::socket> consumer(int receive_buffer_size = 0)
    {
        auto listener = std::make_shared<tcp::acceptor>(source.io(), tcp::endpoint{boost::asio::ip::address_v4::loopback(), 0});
        // 异常退出也由 owner 关闭尚未完成 accept 的局部 listener，不让它阻塞 worker 排空。
        boost::scope::scope_exit close_listener(
            [executor = source.io().get_executor(), listener]()
            {
                boost::asio::post(executor,
                                  [listener]()
                                  {
                                      error_code ignored;
                                      listener->close(ignored);
                                  });
            });
        auto incoming = std::make_shared<tcp::socket>(source.io());
        auto accepted = std::make_shared<std::promise<void>>();
        auto ready = accepted->get_future();
        auto session = receiver;
        boost::asio::post(source.io(),
                          [this, listener, incoming, accepted, session]()
                          {
                              listener->async_accept(
                                  *incoming,
                                  [this, listener, incoming, accepted, session](error_code error)
                                  {
                                      if (error)
                                      {
                                          accepted->set_exception(std::make_exception_ptr(std::runtime_error("HTTP accept")));
                                          return;
                                      }
                                      if (&incoming->get_executor().context() != &source.io())
                                      {
                                          accepted->set_exception(std::make_exception_ptr(std::runtime_error("HTTP source executor")));
                                          return;
                                      }
                                      session->attach(std::move(*incoming), 11);
                                      accepted->set_value();
                                  });
                          });
        auto result = connect(listener->local_endpoint(), receive_buffer_size);
        get_ready(ready);
        return result;
    }

    void write(tcp::socket& socket, const std::vector<std::uint8_t>& bytes)
    {
        const auto error = peer_operation(peer_io, socket, [&](auto done) { boost::asio::async_write(socket, boost::asio::buffer(bytes), done); });
        require(!error, "peer write");
    }

    void fin(tcp::socket& socket)
    {
        error_code error;
        socket.shutdown(tcp::socket::shutdown_send, error);
        require(!error, "device FIN");
    }

    void closed(tcp::socket& socket)
    {
        std::array<char, 1024> buffer{};
        error_code error;
        const auto deadline = std::chrono::steady_clock::now() + operation_limit;
        do
        {
            require(std::chrono::steady_clock::now() < deadline, "peer close deadline");
            error = peer_operation(peer_io, socket, [&](auto done) { socket.async_read_some(boost::asio::buffer(buffer), done); });
        } while (!error);
        require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset, "peer observes socket close");
    }

    void stop_source()
    {
        source.request_stop();
        if (source_thread.joinable())
        {
            source_thread.join();
        }
    }

    void removed_without_shutdown()
    {
        const auto deadline = std::chrono::steady_clock::now() + operation_limit;
        while (session_registry::instance().find_receiver_session(id))
        {
            require(std::chrono::steady_clock::now() < deadline, "disconnect removes receiver without external shutdown");
            // 用有界 owner 屏障推进真实完成回调，不固定休眠。
            on(owner, []() {});
        }
    }

    void finish()
    {
        cleanup();
        require(!session_registry::instance().find_receiver_session(id), "receiver registry released");
        require(weak.expired(), "receiver and pending coroutines released");
        std::vector<std::uint16_t> ports;
        while (const auto port = media_port_pool::instance().acquire())
        {
            ports.push_back(*port);
        }
        std::ranges::sort(ports);
        require(ports == std::vector<std::uint16_t>{20'000, 20'002, 20'004, 20'006}, "all playback leases returned exactly once");
        for (const auto port : ports)
        {
            media_port_pool::instance().release(port);
        }
        tcp::acceptor listener(peer_io);
        listener.open(endpoint.protocol());
        listener.set_option(tcp::acceptor::reuse_address(true));
        listener.bind(endpoint);
        listener.listen();
    }

    worker_context source;
    worker_context owner;
    boost::asio::io_context peer_io;
    const std::string id;
    std::shared_ptr<gb28181_playback_session> receiver;
    std::weak_ptr<gb28181_playback_session> weak;
    tcp::endpoint endpoint;

   private:
    void cleanup() noexcept
    {
        if (receiver)
        {
            if (!owner.stop_requested())
            {
                receiver->shutdown();
            }
            receiver.reset();
        }
        owner.request_stop();
        source.request_stop();
        if (owner_thread.joinable())
        {
            owner_thread.join();
        }
        if (source_thread.joinable())
        {
            source_thread.join();
        }
    }

    inline static std::atomic_uint next_id{};
    std::jthread source_thread;
    std::jthread owner_thread;
};

struct response
{
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;

    void header(fixture& f, tcp::socket& socket, http::status expected = http::status::ok)
    {
        const auto error = peer_operation(f.peer_io, socket, [&](auto done) { http::async_read_header(socket, buffer, parser, done); });
        require(!error && parser.is_header_done(), "HTTP response header");
        require(parser.get().result() == expected, "HTTP response status");
        if (expected == http::status::ok)
        {
            require(parser.chunked(), "HTTP finite stream is chunked");
            require(parser.get()[http::field::content_type] == "video/mpeg", "HTTP raw PS content type");
        }
    }

    error_code body(fixture& f, tcp::socket& socket)
    {
        if (parser.is_done())
        {
            return {};
        }
        return peer_operation(f.peer_io, socket, [&](auto done) { http::async_read(socket, buffer, parser, done); });
    }

    void prefix(fixture& f, tcp::socket& socket, std::string_view expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + operation_limit;
        while (parser.get().body().size() < expected.size())
        {
            require(std::chrono::steady_clock::now() < deadline, "payload prefix deadline");
            const auto error = peer_operation(f.peer_io, socket, [&](auto done) { http::async_read_some(socket, buffer, parser, done); });
            require(!error && !parser.is_done(), "payload prefix before stream completion");
        }
        require(parser.get().body() == expected, "healthy payload received before interruption");
    }
};

std::vector<std::uint8_t> packet(std::uint16_t sequence, std::string_view payload)
{
    const auto length = static_cast<std::uint16_t>(12U + payload.size());
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(length >> 8U),
                                    static_cast<std::uint8_t>(length),
                                    0x80,
                                    96,
                                    static_cast<std::uint8_t>(sequence >> 8U),
                                    static_cast<std::uint8_t>(sequence),
                                    0,
                                    0,
                                    0,
                                    1,
                                    0x12,
                                    0x34,
                                    0x56,
                                    0x78};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& tail) { bytes.insert(bytes.end(), tail.begin(), tail.end()); }

void test_migrate_wrap_device_first()
{
    fixture f;
    auto device = f.device();
    const std::string first("\0\0\1\xba\x65\0", 6);
    const std::string last("\0\0\1\xb9", 4);
    // TCP 建连和首包先于 GET；startup 仅 listen，首包应保留到 attach 开始接收。
    f.write(*device, packet(65535, first));
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    // 停止 G 后再发第二包；错误保留的 G executor 已无法推进后续 IO。
    f.stop_source();
    f.write(*device, packet(0, last));
    f.fin(*device);
    require(!result.body(f, *consumer) && result.parser.is_done(), "normal HTTP chunk terminator");
    require(result.parser.get().body() == first + last, "wrap sequence raw PS bytes, including first and last bytes");
    f.closed(*device);
    f.closed(*consumer);
    f.finish();
}

void test_get_first()
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    auto device = f.device();
    f.write(*device, packet(7, "one-frame"));
    f.fin(*device);
    require(!result.body(f, *consumer) && result.parser.is_done(), "GET before device completes");
    require(result.parser.get().body() == "one-frame", "GET first raw bytes");
    f.closed(*consumer);
    f.finish();
}

enum class malformed
{
    half_length,
    half_payload,
    sequence_gap,
    empty_input
};

void test_incomplete(malformed mode)
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    auto device = f.device();
    std::vector<std::uint8_t> bytes;
    if (mode != malformed::empty_input)
    {
        bytes = packet(10, "valid-prefix");
    }
    switch (mode)
    {
        case malformed::half_length:
            bytes.push_back(0);
            break;
        case malformed::half_payload:
        {
            auto truncated = packet(11, "truncated");
            truncated.pop_back();
            append(bytes, truncated);
            break;
        }
        case malformed::sequence_gap:
            append(bytes, packet(12, "must-not-forward"));
            break;
        case malformed::empty_input:
            break;
    }
    if (!bytes.empty())
    {
        f.write(*device, bytes);
    }
    f.fin(*device);
    require(static_cast<bool>(result.body(f, *consumer)), "truncated/gap stream reports HTTP error");
    require(!result.parser.is_done(), "truncated/gap stream has no HTTP final chunk");
    require(result.parser.get().body() == (mode == malformed::empty_input ? "" : "valid-prefix"), "only validated RTP payload forwarded");
    f.closed(*device);
    f.finish();
}

void test_consumer_disconnect(bool before_device)
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    std::unique_ptr<tcp::socket> device;
    if (!before_device)
    {
        device = f.device();
        f.write(*device, packet(1, "prefix"));
    }
    error_code ignored;
    consumer->close(ignored);    // 完整断开，不是请求发送方向的半关闭。
    if (device)
    {
        f.closed(*device);    // 不再发送设备数据，清理仍应发生。
    }
    f.removed_without_shutdown();
    f.receiver->shutdown();
    f.receiver->shutdown();
    f.finish();
}

void test_second_consumer()
{
    fixture f;
    auto first = f.consumer();
    response accepted;
    accepted.header(f, *first);
    auto second = f.consumer();
    response rejected;
    rejected.header(f, *second, http::status::conflict);
    require(!rejected.body(f, *second) && rejected.parser.is_done(), "second consumer receives complete 409");
    require(session_registry::instance().find_receiver_session(f.id).get() == f.receiver.get(), "409 does not remove original receiver");
    auto device = f.device();
    f.write(*device, packet(1, "original-survives"));
    f.fin(*device);
    require(!accepted.body(f, *first) && accepted.parser.is_done(), "first consumer survives second attach");
    require(accepted.parser.get().body() == "original-survives", "first consumer retains ownership");
    f.finish();
}

class replacement_session final : public session
{
   public:
    void shutdown() override {}
};

void test_stale_identity()
{
    fixture f;
    auto replacement = std::make_shared<replacement_session>();
    auto old = session_registry::instance().take_receiver_session(f.id);
    require(old.get() == f.receiver.get(), "take original receiver");
    old.reset();
    require(session_registry::instance().add_receiver_session(f.id, replacement), "install replacement identity");
    auto consumer = f.consumer();
    response rejected;
    rejected.header(f, *consumer, http::status::conflict);
    require(!rejected.body(f, *consumer) && rejected.parser.is_done(), "stale receiver receives complete 409");
    f.receiver->shutdown();
    on(f.owner, []() {});
    require(session_registry::instance().find_receiver_session(f.id) == replacement, "old cleanup preserves replacement identity");
    session_registry::instance().remove_receiver_session(f.id, *replacement);
    f.finish();
}

void test_owner_already_stopped()
{
    fixture f;
    f.owner.request_stop();
    // stop 标志同步置位；即使目标停止回调尚未执行，attach 也必须释放传入 socket。
    auto consumer = f.consumer();
    f.closed(*consumer);
    f.finish();
}

void test_handoff_queued_then_stop()
{
    fixture f;
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    boost::asio::post(f.owner.io(),
                      [entered, gate]()
                      {
                          entered->set_value();
                          // 即使测试在释放前失败，屏障也有有限期限。
                          static_cast<void>(gate.wait_for(2 * operation_limit));
                      });
    get_ready(entered_future);
    std::unique_ptr<tcp::socket> consumer;
    try
    {
        consumer = f.consumer();    // attach 已排到 owner 屏障之后。
        f.owner.request_stop();
        release->set_value();
    }
    catch (...)
    {
        release->set_value();
        throw;
    }
    f.closed(*consumer);
    f.finish();
}

void test_shutdown_during_transfer(bool stop_owner)
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    auto device = f.device();
    f.write(*device, packet(20, "healthy-before-stop"));
    // 已实际收到正文后才停止，避免只覆盖等待连接或等待首包的路径。
    result.prefix(f, *consumer, "healthy-before-stop");
    if (stop_owner)
    {
        f.owner.request_stop();
    }
    else
    {
        // 模拟 DELETE 已选中本代 receiver 后调用公开 shutdown 的动作。
        f.receiver->shutdown();
    }
    const auto body_error = result.body(f, *consumer);
    require(body_error == http::error::partial_message || body_error == boost::asio::error::eof || body_error == boost::asio::error::connection_reset,
            "interrupted transfer reports incomplete HTTP / network close");
    require(!result.parser.is_done(), "interrupted transfer must not send final chunk");
    require(result.parser.get().body() == "healthy-before-stop", "interruption retains healthy received payload");
    f.closed(*device);
    // HTTP body 读取已观测 consumer 的 EOF/reset；不再等待已消费 EOF 后的新 epoll edge。
    if (!stop_owner)
    {
        f.removed_without_shutdown();
    }
    f.finish();
}

void test_consumer_halfclose_send()
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    auto device = f.device();
    f.write(*device, packet(30, "healthy-before-halfclose"));
    result.prefix(f, *consumer, "healthy-before-halfclose");
    error_code error;
    consumer->shutdown(tcp::socket::shutdown_send, error);
    require(!error, "consumer halfclose send");
    const auto body_error = result.body(f, *consumer);
    require(body_error == http::error::partial_message || body_error == boost::asio::error::eof || body_error == boost::asio::error::connection_reset,
            "consumer halfclose exits with incomplete HTTP / network close");
    require(!result.parser.is_done(), "consumer halfclose must not send final chunk");
    require(result.parser.get().body() == "healthy-before-halfclose", "halfclose retains healthy received payload");
    f.closed(*device);
    // HTTP body 读取已观测 consumer 的 EOF/reset，不重复异步读取同一关闭事件。
    f.removed_without_shutdown();
    f.finish();
}

void test_empty_rtp_payload()
{
    fixture f;
    auto consumer = f.consumer();
    response result;
    result.header(f, *consumer);
    auto device = f.device();
    // RFC4571 长度非零、RTP 头完整，但 PS payload 长度为零。
    f.write(*device, packet(40, {}));
    const auto body_error = result.body(f, *consumer);
    require(body_error == http::error::partial_message || body_error == boost::asio::error::eof || body_error == boost::asio::error::connection_reset,
            "empty RTP payload reports incomplete HTTP / network close");
    require(!result.parser.is_done() && result.parser.get().body().empty(), "empty RTP payload is rejected without final chunk");
    f.closed(*device);
    // HTTP body 读取已观测 consumer 的 EOF/reset，不重复异步读取同一关闭事件。
    f.removed_without_shutdown();
    f.finish();
}

void test_slow_consumer_write_deadline()
{
    fixture f;
    // 在握手前设置 64 KiB 接收缓冲；停读制造背压，不骤缩已建立连接的窗口。
    auto consumer = f.consumer(64 * 1024);
    response result;
    result.parser.body_limit(64U * 1024U * 1024U);
    result.header(f, *consumer);
    auto device = f.device();
    f.write(*device, packet(100, "healthy-before-backpressure"));
    result.prefix(f, *consumer, "healthy-before-backpressure");
    error_code error;

    // 此后不读 consumer，也不发设备 FIN：只有服务端的真实写期限可以正常结束观察。
    // 单一缓冲只在完整写完成后更新序号；没有队列、生产接口或缩短后的测试超时。
    auto bytes = packet(101, std::string(48U * 1024U, 'p'));
    std::uint16_t sequence = 101;
    std::size_t completed_writes{};
    std::array<char, 1> probe{};
    bool failed_deadline{};
    bool server_closed{};
    error_code server_close_error;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::duration::zero();
    f.peer_io.restart();
    boost::asio::steady_timer total(f.peer_io, 30s);
    total.async_wait(
        [&](error_code timer_error)
        {
            if (!timer_error)
            {
                failed_deadline = true;
                error_code ignored;
                device->close(ignored);
                consumer->close(ignored);
            }
        });
    std::function<void()> pump;
    pump = [&]()
    {
        boost::asio::async_write(*device,
                                 boost::asio::buffer(bytes),
                                 [&](error_code write_error, std::size_t count)
                                 {
                                     if (write_error || server_closed || failed_deadline)
                                     {
                                         return;
                                     }
                                     require(count == bytes.size(), "slow consumer complete device packet");
                                     ++completed_writes;
                                     ++sequence;
                                     bytes[4] = static_cast<std::uint8_t>(sequence >> 8U);
                                     bytes[5] = static_cast<std::uint8_t>(sequence);
                                     pump();
                                 });
    };
    device->async_read_some(boost::asio::buffer(probe),
                            [&](error_code read_error, std::size_t)
                            {
                                server_closed = true;
                                server_close_error = read_error;
                                elapsed = std::chrono::steady_clock::now() - started;
                                total.cancel();
                                error_code ignored;
                                device->cancel(ignored);
                            });
    pump();
    f.peer_io.run();

    std::cerr << "[observe] slow_consumer natural_close elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " completed_device_packets=" << completed_writes << " server_closed=" << server_closed
              << " close_error=" << server_close_error.message() << " failure_timer=" << failed_deadline
              << " registry_present=" << static_cast<bool>(session_registry::instance().find_receiver_session(f.id))
              << " body=" << result.parser.get().body().size() << " buffered=" << result.buffer.size() << '\n';
    require(!failed_deadline && server_closed, "server closes naturally before failure-only deadline");
    require(server_close_error == boost::asio::error::eof || server_close_error == boost::asio::error::connection_reset,
            "device observes passive server close");
    require(elapsed >= 19s && elapsed <= 28s, "real 20 second write deadline observation window");
    require(completed_writes > 10, "multiple healthy packets precede write timeout");
    // 先观察服务端自然清理，再恢复读取；未读 parser 的状态不能证明线上没有最终 chunk。
    f.removed_without_shutdown();
    consumer->set_option(boost::asio::socket_base::receive_buffer_size(256 * 1024), error);
    require(!error, "restore consumer receive buffer for final drain");
    boost::asio::socket_base::receive_buffer_size receive_buffer;
    consumer->get_option(receive_buffer, error);
    require(!error, "read consumer receive buffer for drain observation");
    const auto available = consumer->available(error);
    require(!error, "read consumer queued bytes for drain observation");
    std::cerr << "[observe] slow_consumer drain_start recvbuf=" << receive_buffer.value() << " kernel_available=" << available
              << " body=" << result.parser.get().body().size() << " buffered=" << result.buffer.size() << '\n';
    const auto drain_started = std::chrono::steady_clock::now();
    error_code drain_error;
    try
    {
        drain_error = peer_operation(f.peer_io, *consumer, [&](auto done) { http::async_read(*consumer, result.buffer, result.parser, done); }, 10s);
    }
    catch (...)
    {
        std::cerr << "[observe] slow_consumer drain_exception elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drain_started).count()
                  << " body=" << result.parser.get().body().size() << " buffered=" << result.buffer.size()
                  << " parser_done=" << result.parser.is_done() << " consumer_open=" << consumer->is_open()
                  << " registry_present=" << static_cast<bool>(session_registry::instance().find_receiver_session(f.id)) << '\n';
        throw;
    }
    std::cerr << "[observe] slow_consumer drain_end elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drain_started).count()
              << " error=" << drain_error.message() << " body=" << result.parser.get().body().size() << " buffered=" << result.buffer.size()
              << " parser_done=" << result.parser.is_done() << '\n';
    require(
        drain_error == http::error::partial_message || drain_error == boost::asio::error::connection_reset || drain_error == boost::asio::error::eof,
        "write deadline produces actual incomplete HTTP / network error");
    require(!result.parser.is_done(), "write deadline does not send final HTTP chunk");
    require(result.parser.get().body().starts_with("healthy-before-backpressure"), "final drain retains healthy received prefix");
    std::cout << "[observe] slow_consumer elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " completed_device_packets=" << completed_writes << " received_body_bytes=" << result.parser.get().body().size()
              << " healthy_prefix=healthy-before-backpressure\n";
    // 服务端自行关闭和 HTTP 不完整结束均已确认，才允许清理剩余客户端资源。
    error_code ignored;
    consumer->close(ignored);
    device->close(ignored);
    f.finish();
}

void test_pool_exhaustion()
{
    worker_context owner;
    std::jthread runner([&]() { owner.run(); });
    boost::scope::scope_exit stop(
        [&]()
        {
            owner.request_stop();
            runner.join();
        });
    std::vector<std::shared_ptr<gb28181_playback_session>> sessions;
    std::vector<std::uint16_t> ports;
    on(owner,
       [&]()
       {
           for (int index = 0; index < 4; ++index)
           {
               auto receiver = std::make_shared<gb28181_playback_session>(owner, "pool-" + std::to_string(index), 96, test_ssrc);
               const auto port = receiver->startup(boost::asio::ip::address_v4::loopback());
               require(port.has_value(), "allocate each available playback port");
               ports.push_back(*port);
               sessions.push_back(std::move(receiver));
           }
           auto extra = std::make_shared<gb28181_playback_session>(owner, "pool-exhausted", 96, test_ssrc);
           require(!extra->startup(boost::asio::ip::address_v4::loopback()), "exhausted pool rejects playback startup");
           extra->shutdown();
       });
    boost::asio::io_context peer;
    for (const auto port : ports)
    {
        tcp::socket socket(peer);
        socket.connect({boost::asio::ip::address_v4::loopback(), port});
    }
    for (const auto& receiver : sessions)
    {
        receiver->shutdown();
        receiver->shutdown();
    }
    on(owner, []() {});
    for (int index = 0; index < 4; ++index)
    {
        require(media_port_pool::instance().acquire().has_value(), "exhaustion cleanup restores every lease");
    }
    require(!media_port_pool::instance().acquire(), "exhaustion cleanup did not duplicate lease");
    for (const auto port : ports)
    {
        media_port_pool::instance().release(port);
    }
}

void test_bind_failure_and_reused_lease()
{
    auto& pool = media_port_pool::instance();
    const auto port = pool.acquire();
    require(port.has_value(), "reserve conflict target");
    pool.release(*port);
    worker_context owner;
    std::jthread runner([&]() { owner.run(); });
    boost::scope::scope_exit stop(
        [&]()
        {
            owner.request_stop();
            runner.join();
        });
    boost::asio::io_context peer;
    tcp::acceptor blocker(peer, {boost::asio::ip::address_v4::loopback(), *port});
    auto previous = std::make_shared<gb28181_playback_session>(owner, "bind-failure", 96, test_ssrc);
    on(owner, [&]() { require(!previous->startup(boost::asio::ip::address_v4::loopback()), "bind failure must not retry another pool entry"); });
    std::vector<std::uint16_t> available;
    while (const auto value = pool.acquire())
    {
        require(value != port, "failed startup retains its lease until shutdown");
        available.push_back(*value);
    }
    require(available.size() == 3, "bind failure acquired exactly one lease");
    for (const auto value : available)
    {
        pool.release(value);
    }
    previous->shutdown();
    on(owner, []() {});
    blocker.close();
    auto current = std::make_shared<gb28181_playback_session>(owner, "reused-port", 96, test_ssrc);
    on(owner, [&]() { require(current->startup(boost::asio::ip::address_v4::loopback()) == port, "new owner reuses released port"); });
    previous->shutdown();
    previous->shutdown();
    on(owner, []() {});
    available.clear();
    while (const auto value = pool.acquire())
    {
        require(value != port, "old shutdown must not return new owner's lease");
        available.push_back(*value);
    }
    require(available.size() == 3, "new owner still owns exactly one lease");
    tcp::socket connection(peer);
    connection.connect({boost::asio::ip::address_v4::loopback(), *port});
    connection.close();
    current->shutdown();
    on(owner, []() {});
    const auto returned = pool.acquire();
    require(returned == port && !pool.acquire(), "current shutdown returns its lease exactly once");
    pool.release(*returned);
    for (const auto value : available)
    {
        pool.release(value);
    }
}

}    // namespace
}    // namespace media_server

int main()
{
    using namespace media_server;
    media_port_pool::init(20'000, 20'007);
    const std::array<std::pair<const char*, void (*)()>, 19> cases{{
        {"migrate_wrap_device_first", test_migrate_wrap_device_first},
        {"get_first", test_get_first},
        {"half_length", []() { test_incomplete(malformed::half_length); }},
        {"half_payload", []() { test_incomplete(malformed::half_payload); }},
        {"sequence_gap", []() { test_incomplete(malformed::sequence_gap); }},
        {"empty_input", []() { test_incomplete(malformed::empty_input); }},
        {"consumer_disconnect_before_device", []() { test_consumer_disconnect(true); }},
        {"consumer_disconnect_silent_device", []() { test_consumer_disconnect(false); }},
        {"second_consumer", test_second_consumer},
        {"stale_identity", test_stale_identity},
        {"owner_already_stopped", test_owner_already_stopped},
        {"handoff_queued_then_stop", test_handoff_queued_then_stop},
        {"shutdown_during_transfer", []() { test_shutdown_during_transfer(false); }},
        {"owner_stop_during_transfer", []() { test_shutdown_during_transfer(true); }},
        {"consumer_halfclose_send", test_consumer_halfclose_send},
        {"empty_rtp_payload", test_empty_rtp_payload},
        {"slow_consumer_write_deadline", test_slow_consumer_write_deadline},
        {"pool_exhaustion", test_pool_exhaustion},
        {"bind_failure_and_reused_lease", test_bind_failure_and_reused_lease},
    }};
    for (const auto& [name, function] : cases)
    {
        try
        {
            function();
            std::cout << "[pass] " << name << '\n';
        }
        catch (const std::exception& error)
        {
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
