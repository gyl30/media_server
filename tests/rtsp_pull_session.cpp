#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_pull_session.h"

namespace
{
using namespace media_server;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// 关闭执行时 DNS 成功结果已排队：恢复的协程不能让已关闭的会话再去连接上游。
// 若调度使连接先于关闭建立，关闭也必须断开它；不允许出现保持着的连接。
void late_resolve_after_shutdown()
{
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(server_io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    acceptor.non_blocking(true);
    const auto port = acceptor.local_endpoint().port();

    worker_context worker;
    std::thread thread([&worker]() { worker.run(); });
    const auto session = std::make_shared<rtsp_pull_session>(worker, "live/late", "rtsp://127.0.0.1:" + std::to_string(port) + "/late");
    session->startup();
    // 阻塞 worker：关闭排队期间后台解析完成，其成功结果排在关闭之后，关闭时已无法取消。
    boost::asio::post(worker.io(), []() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); });
    session->shutdown();

    bool held = false;
    boost::asio::ip::tcp::socket peer(server_io);
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        boost::system::error_code error;
        acceptor.accept(peer, error);
        if (!error)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer.is_open())
    {
        // 连接存在时，会话必须在关闭后断开它（对端读到 EOF），而不是继续发送 DESCRIBE 保持连接。
        peer.non_blocking(true);
        held = true;
        std::array<char, 1024> buffer{};
        for (int attempt = 0; attempt < 100 && held; ++attempt)
        {
            boost::system::error_code error;
            for (;;)
            {
                peer.read_some(boost::asio::buffer(buffer), error);
                if (error)
                {
                    break;
                }
            }
            if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset)
            {
                held = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    worker.request_stop();
    thread.join();
    require(!held, "closed session connected upstream and kept the connection");
    std::cout << "late_resolve_after_shutdown: PASS\n";
}
}    // namespace

int main()
{
    try
    {
        late_resolve_after_shutdown();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
