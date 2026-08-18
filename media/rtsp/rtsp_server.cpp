#include "media/rtsp/rtsp_server.h"

#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_server_connection.h"
#include "media/rtsp/rtsp_output_session.h"

#include <boost/asio/write.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace media_server
{

class rtsp_connection_router final : public std::enable_shared_from_this<rtsp_connection_router>
{
   public:
    rtsp_connection_router(std::weak_ptr<rtsp_server> owner, boost::asio::ip::tcp::socket socket)
        : owner_(std::move(owner)), socket_(std::move(socket)), executor_(socket_.get_executor())
    {
    }

    void startup() { read_next(); }

    void shutdown()
    {
        const auto self = shared_from_this();
        boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
    }

   private:
    void read_next()
    {
        if (closed_ || routed_ || writing_)
        {
            return;
        }
        const auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
                                [self](const boost::system::error_code& error, std::size_t bytes)
                                {
                                    if (error || bytes == 0)
                                    {
                                        self->close();
                                        return;
                                    }
                                    self->buffer_.insert(self->buffer_.end(), self->read_buffer_.begin(), self->read_buffer_.begin() + bytes);
                                    self->process();
                                });
    }

    void process()
    {
        const std::string_view data(reinterpret_cast<const char*>(buffer_.data()), buffer_.size());
        const auto header_end = data.find("\r\n\r\n");
        if (header_end == std::string_view::npos)
        {
            if (buffer_.size() > 64U * 1024U)
            {
                close();
                return;
            }
            read_next();
            return;
        }

        const auto line_end = data.find("\r\n");
        if (line_end == std::string_view::npos)
        {
            close();
            return;
        }
        const auto space = data.find(' ');
        if (space == std::string_view::npos || space > line_end)
        {
            close();
            return;
        }
        const auto method = data.substr(0, space);
        if (boost::iequals(method, "OPTIONS"))
        {
            std::optional<unsigned int> cseq;
            auto header_offset = line_end + 2U;
            while (header_offset < header_end)
            {
                const auto header_line_end = data.find("\r\n", header_offset);
                if (header_line_end == std::string_view::npos || header_line_end > header_end)
                {
                    close();
                    return;
                }
                auto line = data.substr(header_offset, header_line_end - header_offset);
                const auto colon = line.find(':');
                if (colon != std::string_view::npos && boost::iequals(line.substr(0, colon), "CSeq"))
                {
                    auto value = line.substr(colon + 1U);
                    const auto first = value.find_first_not_of(" \t");
                    if (first == std::string_view::npos)
                    {
                        close();
                        return;
                    }
                    value.remove_prefix(first);
                    const auto last = value.find_first_of(" \t");
                    value = value.substr(0, last);
                    unsigned int parsed{};
                    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if (error != std::errc{} || pointer != value.data() + value.size())
                    {
                        close();
                        return;
                    }
                    cseq = parsed;
                    break;
                }
                header_offset = header_line_end + 2U;
            }
            if (!cseq)
            {
                close();
                return;
            }
            const auto consumed = header_end + 4U;
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
            pending_write_ = "RTSP/1.0 200 OK\r\nCSeq: " + std::to_string(*cseq) +
                             "\r\nPublic: OPTIONS,DESCRIBE,SETUP,TEARDOWN,PLAY,ANNOUNCE,RECORD,GET_PARAMETER\r\nContent-Length: 0\r\n\r\n";
            writing_ = true;
            const auto self = shared_from_this();
            boost::asio::async_write(socket_, boost::asio::buffer(pending_write_), [self](const boost::system::error_code& write_error, std::size_t)
                                     {
                                         if (write_error || self->closed_)
                                         {
                                             self->close();
                                             return;
                                         }
                                         self->writing_ = false;
                                         self->pending_write_.clear();
                                         self->process();
                                     });
            return;
        }

        if (!boost::iequals(method, "ANNOUNCE") && !boost::iequals(method, "DESCRIBE") && !boost::iequals(method, "SETUP"))
        {
            close();
            return;
        }
        const auto owner = owner_.lock();
        if (!owner)
        {
            close();
            return;
        }
        routed_ = true;
        closed_ = true;
        owner->on_connection(std::move(socket_), std::move(buffer_), boost::iequals(method, "ANNOUNCE"));
    }

    void close()
    {
        safe_shutdown();
    }

    void safe_shutdown()
    {
        if (closed_)
        {
            return;
        }
        closed_ = true;
        boost::system::error_code error;
        socket_.cancel(error);
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
        socket_.close(error);
        buffer_.clear();
    }

    std::weak_ptr<rtsp_server> owner_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::any_io_executor executor_;
    std::array<std::uint8_t, 16 * 1024> read_buffer_{};
    std::vector<std::uint8_t> buffer_;
    std::string pending_write_;
    bool writing_{};
    bool routed_{};
    bool closed_{};
};

rtsp_server::rtsp_server(io_context_pool& workers, stream_registry& registry, std::uint16_t port, output_video_config video)
    : registry_(registry), video_config_(video), listener_(std::make_shared<tcp_listener>(workers, port))
{
}

boost::system::error_code rtsp_server::startup()
{
    const std::weak_ptr<rtsp_server> weak = shared_from_this();
    return listener_->startup(
        [weak](boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(std::move(socket));
            }
        });
}

void rtsp_server::shutdown()
{
    std::vector<std::weak_ptr<rtsp_connection_router>> routers;
    std::vector<std::weak_ptr<rtsp_server_connection>> connections;
    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            return;
        }
        closed_ = true;
        routers = std::move(routers_);
        routers_.clear();
        connections = std::move(connections_);
        connections_.clear();
    }
    listener_->shutdown();
    for (const auto& weak_router : routers)
    {
        if (const auto router = weak_router.lock())
        {
            router->shutdown();
        }
    }
    for (const auto& weak_connection : connections)
    {
        if (const auto connection = weak_connection.lock())
        {
            connection->shutdown();
        }
    }
}

void rtsp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    std::shared_ptr<rtsp_connection_router> router;
    {
        std::scoped_lock lock(sessions_mutex_);
        if (closed_)
        {
            boost::system::error_code error;
            socket.close(error);
            return;
        }
        router = std::make_shared<rtsp_connection_router>(shared_from_this(), std::move(socket));
        std::erase_if(routers_, [](const auto& value) { return value.expired(); });
        routers_.emplace_back(router);
    }
    router->startup();
}

void rtsp_server::on_connection(boost::asio::ip::tcp::socket socket, std::vector<std::uint8_t> initial_data, bool publish)
{
    std::scoped_lock lock(sessions_mutex_);
    if (closed_)
    {
        boost::system::error_code error;
        socket.close(error);
        return;
    }

    auto tcp = std::make_shared<tcp_connection>(std::move(socket));
    auto connection = std::make_shared<rtsp_server_connection>(std::move(tcp));
    std::erase_if(connections_, [](const auto& value) { return value.expired(); });
    connections_.emplace_back(connection);

    if (publish)
    {
        auto session = std::make_shared<rtsp_input_session>(connection, registry_, std::move(initial_data));
        session->startup();
        return;
    }

    auto session = std::make_shared<rtsp_output_session>(connection, registry_, video_config_);
    session->startup(std::move(initial_data));
}

}    // namespace media_server
