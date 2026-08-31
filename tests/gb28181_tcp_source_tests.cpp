#include <chrono>
#include <cerrno>
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
    boost::system::error_code startup_error;
    connector->startup(
        [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
        {
            ++completion_count;
            completion_error = error;
            require(!socket.is_open(), "refused connector has no connected socket");
            connector->shutdown();
        },
        startup_error);
    require(!startup_error, "connector startup");
    io.run();

    require(completion_count == 1, "connector completes once");
    require(static_cast<bool>(completion_error), "connector reports terminal error");
}

void test_acceptor_timeout_reports_terminal_error_without_shutdown()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reserved.local_endpoint();
    reserved.close();

    int completion_count = 0;
    boost::system::error_code completion_error;
    auto acceptor =
        std::make_shared<tcp_acceptor>(io.get_executor(), endpoint.port(), boost::asio::ip::address_v4::loopback(), std::chrono::milliseconds(5));
    boost::system::error_code startup_error;
    acceptor->startup(
        [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
        {
            ++completion_count;
            completion_error = error;
            require(!socket.is_open(), "timed out acceptor has no connected socket");
        },
        startup_error);
    require(!startup_error, "acceptor startup");
    io.run();

    require(completion_count == 1, "acceptor timeout completes once");
    require(completion_error == boost::asio::error::timed_out, "acceptor timeout reports timed_out");

    boost::asio::ip::tcp::acceptor before_shutdown(io);
    boost::system::error_code before_shutdown_error;
    before_shutdown.open(boost::asio::ip::tcp::v4(), before_shutdown_error);
    before_shutdown.bind(endpoint, before_shutdown_error);
    require(before_shutdown_error == boost::asio::error::address_in_use, "acceptor timeout keeps listener resource until owner shutdown");

    acceptor->shutdown();
    io.restart();
    io.run();

    boost::asio::ip::tcp::acceptor after_shutdown(io);
    boost::system::error_code after_shutdown_error;
    after_shutdown.open(boost::asio::ip::tcp::v4(), after_shutdown_error);
    after_shutdown.bind(endpoint, after_shutdown_error);
    require(!after_shutdown_error, "acceptor owner shutdown releases listener resource");
}

void test_acceptor_reports_success()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reserved.local_endpoint();
    reserved.close();

    int completion_count = 0;
    auto acceptor =
        std::make_shared<tcp_acceptor>(io.get_executor(), endpoint.port(), boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    boost::asio::ip::tcp::socket client(io);
    boost::system::error_code startup_error;
    acceptor->startup(
        [&](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
        {
            ++completion_count;
            require(!error && socket.is_open(), "acceptor reports connected socket");
            boost::system::error_code ignored;
            socket.close(ignored);
        },
        startup_error);
    require(!startup_error, "acceptor success startup");

    boost::asio::ip::tcp::acceptor other_address(io);
    boost::system::error_code other_bind_error;
    other_address.open(boost::asio::ip::tcp::v4(), other_bind_error);
    other_address.bind({boost::asio::ip::make_address_v4("127.0.0.2"), endpoint.port()}, other_bind_error);
    require(!other_bind_error, "acceptor only binds requested local address");
    client.connect(endpoint);
    io.run();

    require(completion_count == 1, "acceptor success completes once");
    boost::system::error_code restart_error;
    acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; }, restart_error);
    require(restart_error == boost::asio::error::already_started, "acceptor cannot restart after completion");
    require(completion_count == 1, "acceptor restart has no completion");
}

void test_acceptor_reports_bind_failure()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reserved(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reserved.local_endpoint();
    int completion_count = 0;
    auto acceptor =
        std::make_shared<tcp_acceptor>(io.get_executor(), endpoint.port(), boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    boost::system::error_code startup_error;
    acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; }, startup_error);
    require(startup_error == boost::asio::error::address_in_use, "acceptor reports bind failure");
    io.run();
    require(completion_count == 0, "acceptor bind failure has no completion");
}

void test_acceptor_rejects_invalid_local_address()
{
    boost::asio::io_context io;
    boost::system::error_code startup_error;
    auto unspecified =
        std::make_shared<tcp_acceptor>(io.get_executor(), 0, boost::asio::ip::address_v4::any(), std::chrono::seconds(1));
    unspecified->startup([](boost::system::error_code, boost::asio::ip::tcp::socket) {}, startup_error);
    require(startup_error == boost::asio::error::invalid_argument, "acceptor rejects unspecified address");

    auto unavailable =
        std::make_shared<tcp_acceptor>(io.get_executor(), 0, boost::asio::ip::make_address("192.0.2.1"), std::chrono::seconds(1));
    unavailable->startup([](boost::system::error_code, boost::asio::ip::tcp::socket) {}, startup_error);
    require(startup_error.value() == EADDRNOTAVAIL, "acceptor preserves unavailable address error");
}

void test_pending_source_shutdown_suppresses_completion()
{
    boost::asio::io_context io;
    int completion_count = 0;
    auto acceptor = std::make_shared<tcp_acceptor>(io.get_executor(), 0, boost::asio::ip::address_v4::loopback(), std::chrono::seconds(1));
    boost::system::error_code startup_error;
    acceptor->startup([&](boost::system::error_code, boost::asio::ip::tcp::socket) { ++completion_count; }, startup_error);
    require(!startup_error, "pending acceptor startup");
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
        media_server::test_acceptor_rejects_invalid_local_address();
        media_server::test_acceptor_timeout_reports_terminal_error_without_shutdown();
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
