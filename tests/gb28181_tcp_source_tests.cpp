#include <chrono>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "media/net/tcp_acceptor.h"
#include "media/net/tcp_connector.h"

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

void test_connector_reports_terminal_error_once()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io);
    reserved.open(boost::asio::ip::tcp::v4());
    reserved.bind({boost::asio::ip::address_v4::loopback(), 0});

    int completion_count = 0;
    boost::system::error_code completion_error;
    auto connector = std::make_shared<tcp_connector>(io.get_executor(), reserved.local_endpoint(), std::chrono::seconds(1));
    require(!connector->startup(
                [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
                {
                    ++completion_count;
                    completion_error = error;
                    require(!socket.is_open(), "refused connector socket closed");
                }),
            "connector startup");
    io.run();

    require(completion_count == 1, "connector completes once");
    require(static_cast<bool>(completion_error), "connector reports terminal error");
}

void test_acceptor_timeout_reports_terminal_error()
{
    boost::asio::io_context io;
    int completion_count = 0;
    boost::system::error_code completion_error;
    auto acceptor = std::make_shared<tcp_acceptor>(io.get_executor(), 0, boost::asio::ip::address_v4::loopback(), std::chrono::milliseconds(5));
    require(!acceptor->startup(
                [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
                {
                    ++completion_count;
                    completion_error = error;
                    require(!socket.is_open(), "timed out acceptor socket closed");
                }),
            "acceptor startup");
    io.run();

    require(completion_count == 1, "acceptor timeout completes once");
    require(completion_error == boost::asio::error::timed_out, "acceptor timeout reports timed_out");
}

void test_acceptor_reports_success()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io, {boost::asio::ip::tcp::v4(), 0});
    const auto endpoint = reserved.local_endpoint();
    reserved.close();

    int completion_count = 0;
    auto acceptor =
        std::make_shared<tcp_acceptor>(io.get_executor(), endpoint.port(), boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    boost::asio::ip::tcp::socket client(io);
    require(!acceptor->startup(
                [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
                {
                    ++completion_count;
                    require(!error && socket.is_open(), "acceptor reports connected socket");
                    boost::system::error_code ignored;
                    socket.close(ignored);
                }),
            "acceptor success startup");
    client.connect(endpoint);
    io.run();

    require(completion_count == 1, "acceptor success completes once");
    require(acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; }) ==
                boost::asio::error::already_started,
            "acceptor cannot restart after completion");
    require(completion_count == 1, "acceptor restart has no completion");
}

void test_acceptor_reports_bind_failure()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io, {boost::asio::ip::tcp::v4(), 0});
    const auto endpoint = reserved.local_endpoint();
    int completion_count = 0;
    auto acceptor =
        std::make_shared<tcp_acceptor>(io.get_executor(), endpoint.port(), boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    const auto error = acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; });
    require(error == boost::asio::error::address_in_use, "acceptor reports bind failure");
    io.run();
    require(completion_count == 0, "acceptor bind failure has no completion");
}

void test_pending_source_shutdown_suppresses_completion()
{
    boost::asio::io_context io;
    int completion_count = 0;
    auto acceptor = std::make_shared<tcp_acceptor>(io.get_executor(), 0, boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    require(!acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; }), "pending acceptor startup");
    acceptor->shutdown();
    io.run();
    require(completion_count == 0, "acceptor shutdown suppresses completion");
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_connector_reports_terminal_error_once();
        media_server::test_acceptor_reports_success();
        media_server::test_acceptor_reports_bind_failure();
        media_server::test_acceptor_timeout_reports_terminal_error();
        media_server::test_pending_source_shutdown_suppresses_completion();
        std::cout << "[pass] gb28181_tcp_source_tests\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] gb28181_tcp_source_tests: " << error.what() << '\n';
        return 1;
    }
}
