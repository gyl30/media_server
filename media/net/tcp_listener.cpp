#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/bind_executor.hpp>

#include "media/net/tcp_listener.h"

namespace media_server
{

tcp_listener::tcp_listener(io_context_pool& workers, std::uint16_t port, boost::asio::ip::address bind_address)
    : acceptor_(workers.context(0).io()), timer_(workers.context(0).io()), workers_(workers), bind_address_(std::move(bind_address)), port_(port)
{
}

void tcp_listener::startup(accept_handler handler, std::size_t accept_limit, std::chrono::milliseconds timeout, boost::system::error_code& error)
{
    error.clear();
    if (started_)
    {
        return;
    }
    if (bind_address_.is_unspecified())
    {
        error = boost::asio::error::invalid_argument;
        return;
    }

    const boost::asio::ip::tcp::endpoint endpoint{
        bind_address_,
        port_,
    };
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
        return;
    }

    started_ = true;
    accepting_ = true;
    accept_handler_ = std::move(handler);
    timeout_ = timeout;
    accept_limit_ = accept_limit;
    accepted_count_ = 0;
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(),
                      [this, self]()
                      {
                          accept_next();
                          schedule_timeout();
                      });
}

void tcp_listener::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(acceptor_.get_executor(), [this, self]() { safe_shutdown(); });
}

void tcp_listener::schedule_timeout()
{
    if (!started_ || !accepting_ || timeout_ <= std::chrono::milliseconds::zero())
    {
        return;
    }

    timer_.expires_after(timeout_);
    const auto self = shared_from_this();
    timer_.async_wait([this, self](const boost::system::error_code& error) { on_timeout(error); });
}

void tcp_listener::on_timeout(const boost::system::error_code& error)
{
    if (error || !started_ || !accepting_)
    {
        return;
    }

    accepting_ = false;
    auto* worker = accepting_worker_;
    accepting_worker_ = nullptr;
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    auto handler = std::move(accept_handler_);
    if (handler)
    {
        handler(boost::asio::error::make_error_code(boost::asio::error::timed_out), *worker, boost::asio::ip::tcp::socket{worker->io()});
    }
}

void tcp_listener::safe_shutdown()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    accepting_ = false;
    accepting_worker_ = nullptr;
    accept_handler_ = {};
    timer_.cancel();
    boost::system::error_code error;
    acceptor_.cancel(error);
    acceptor_.close(error);
}

void tcp_listener::accept_next()
{
    if (!started_ || !accepting_)
    {
        return;
    }

    auto* worker = &workers_.next();
    accepting_worker_ = worker;
    const auto self = shared_from_this();
    acceptor_.async_accept(
        worker->io(),
        boost::asio::bind_executor(acceptor_.get_executor(),
                                   [this, self, worker](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
                                   { on_accept(*worker, error, std::move(socket)); }));
}

void tcp_listener::on_accept(worker_context& worker, const boost::system::error_code& error, boost::asio::ip::tcp::socket socket)
{
    if (!started_ || !accepting_)
    {
        boost::system::error_code ignored;
        socket.close(ignored);
        return;
    }
    if (error)
    {
        accepting_ = false;
        accepting_worker_ = nullptr;
        timer_.cancel();
        auto handler = std::move(accept_handler_);
        if (handler)
        {
            const auto self = shared_from_this();
            auto* accepted_worker = &worker;
            boost::asio::dispatch(acceptor_.get_executor(),
                                  [this, self, accepted_worker, handler = std::move(handler), error]() mutable
                                  { handler(error, *accepted_worker, boost::asio::ip::tcp::socket{accepted_worker->io()}); });
        }
        return;
    }

    ++accepted_count_;
    auto handler = accept_handler_;
    if (accept_limit_ != 0 && accepted_count_ >= accept_limit_)
    {
        safe_shutdown();
    }
    else
    {
        accept_next();
    }
    if (handler)
    {
        const auto self = shared_from_this();
        auto* accepted_worker = &worker;
        boost::asio::dispatch(socket.get_executor(),
                              [this, self, accepted_worker, handler = std::move(handler), socket = std::move(socket)]() mutable
                              { handler({}, *accepted_worker, std::move(socket)); });
    }
}

}    // namespace media_server
