#include <cerrno>
#include <utility>

#include <boost/asio/error.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

accept_error_action classify_accept_error(const boost::system::error_code& error) noexcept
{
    if (error.category() != boost::system::system_category())
    {
        return accept_error_action::fatal;
    }
    switch (error.value())
    {
        // 资源耗尽在其他连接释放后恢复，需要退避避免空转。
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            return accept_error_action::retry_later;
        // accept(2)：已完成握手的单个连接出错，监听 socket 本身仍然可用。
        case ECONNABORTED:
        case EPROTO:
        case EPERM:
        case EINTR:
        case ENETDOWN:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return accept_error_action::retry_now;
        default:
            return accept_error_action::fatal;
    }
}

tcp_listener::tcp_listener(boost::asio::io_context& io, std::uint16_t port, boost::asio::ip::address bind_address)
    : acceptor_(io), port_(port), bind_address_(std::move(bind_address))
{
}

void tcp_listener::startup(boost::system::error_code& error)
{
    error.clear();
    if (bind_address_.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{bind_address_, port_};
    acceptor_.open(endpoint.protocol(), error);
    if (!error)
    {
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
    }
    if (!error)
    {
        acceptor_.bind(endpoint, error);
    }
    if (!error)
    {
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
    }
    if (error)
    {
        boost::system::error_code close_error;
        acceptor_.close(close_error);
    }
}

void tcp_listener::accept(boost::asio::ip::tcp::socket& socket,
                          boost::asio::yield_context& yield,
                          boost::system::error_code& error)
{
    error.clear();
    acceptor_.async_accept(socket, yield[error]);
}

void tcp_listener::shutdown()
{
    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

}    // namespace media_server
