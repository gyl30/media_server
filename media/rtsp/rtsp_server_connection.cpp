#include "media/rtsp/rtsp_server_connection.h"

#include <boost/asio/post.hpp>

#include <string>
#include <utility>

namespace media_server
{

rtsp_server_connection::rtsp_server_connection(std::shared_ptr<tcp_connection> connection) : connection_(std::move(connection)) {}

rtsp_server_connection::~rtsp_server_connection()
{
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
    }
}

bool rtsp_server_connection::startup(std::shared_ptr<const rtsp_server_connection_handler> handler, std::vector<std::uint8_t> initial_data)
{
    if (closed_ || connection_ == nullptr || server_ != nullptr)
    {
        return false;
    }

    boost::system::error_code endpoint_error;
    const auto peer = connection_->socket().remote_endpoint(endpoint_error);
    if (endpoint_error)
    {
        return false;
    }
    const auto local = connection_->socket().local_endpoint(endpoint_error);
    if (endpoint_error)
    {
        return false;
    }

    handler_ = std::move(handler);
    rtsp_handler_t rtsp_handler{};
    rtsp_handler.send = &rtsp_server_connection::send_callback;
    rtsp_handler.ondescribe = handler_->on_describe ? &rtsp_server_connection::describe_callback : nullptr;
    rtsp_handler.onsetup = handler_->on_setup ? &rtsp_server_connection::setup_callback : nullptr;
    rtsp_handler.onplay = handler_->on_play ? &rtsp_server_connection::play_callback : nullptr;
    rtsp_handler.onteardown = handler_->on_teardown ? &rtsp_server_connection::teardown_callback : nullptr;
    rtsp_handler.onannounce = handler_->on_announce ? &rtsp_server_connection::announce_callback : nullptr;
    rtsp_handler.onrecord = handler_->on_record ? &rtsp_server_connection::record_callback : nullptr;
    rtsp_handler.onoptions = &rtsp_server_connection::options_callback;
    rtsp_handler.ongetparameter = handler_->on_get_parameter ? &rtsp_server_connection::get_parameter_callback : nullptr;

    const auto peer_address = peer.address().to_string();
    server_ = rtsp_server_create(peer_address.c_str(), peer.port(), &rtsp_handler, this, this);
    if (server_ == nullptr)
    {
        handler_.reset();
        return false;
    }
    local_address_ = local.address().to_string();

    const auto self = shared_from_this();
    connection_->startup(
        [self](boost::system::error_code error, std::span<const std::uint8_t> data)
        {
            if (error)
            {
                self->shutdown();
                return;
            }
            self->on_tcp_read(data);
        },
        [self](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                self->shutdown();
            }
        });

    if (!initial_data.empty())
    {
        on_tcp_read(initial_data);
    }
    return !closed_;
}

void rtsp_server_connection::set_handler(std::shared_ptr<const rtsp_server_connection_handler> handler) { handler_ = std::move(handler); }

std::size_t rtsp_server_connection::input(std::span<const std::uint8_t> data)
{
    if (closed_ || server_ == nullptr || data.empty())
    {
        return 0;
    }

    auto bytes = data.size();
    const auto result = rtsp_server_input(server_, data.data(), &bytes);
    if (result < 0)
    {
        shutdown();
        return data.size();
    }

    const auto consumed = data.size() - bytes;
    if (result == 0 && consumed == 0)
    {
        shutdown();
        return data.size();
    }
    return consumed;
}

void rtsp_server_connection::write(std::span<const std::uint8_t> data)
{
    if (!closed_ && connection_ && !data.empty())
    {
        connection_->write(data);
    }
}

void rtsp_server_connection::shutdown()
{
    if (connection_ == nullptr)
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(connection_->socket().get_executor(), [self]() { self->safe_shutdown(); });
}

boost::asio::any_io_executor rtsp_server_connection::executor() const { return connection_->socket().get_executor(); }

std::string rtsp_server_connection::local_address() const { return local_address_; }

int rtsp_server_connection::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_ || self->connection_ == nullptr)
    {
        return -1;
    }
    self->connection_->write(data, bytes);
    return 0;
}

int rtsp_server_connection::describe_callback(void* param, rtsp_server_t* server, const char* uri)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_describe ? handler->on_describe(server, uri != nullptr ? uri : "") : rtsp_server_reply_describe(server, 501, "");
}

int rtsp_server_connection::setup_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_setup
               ? handler->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count)
               : rtsp_server_reply_setup(server, 501, nullptr, nullptr);
}

int rtsp_server_connection::play_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_play
               ? handler->on_play(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale)
               : rtsp_server_reply_play(server, 501, nullptr, nullptr, nullptr);
}

int rtsp_server_connection::teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_teardown
               ? handler->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "")
               : rtsp_server_reply_teardown(server, 501);
}

int rtsp_server_connection::announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_announce ? handler->on_announce(server, uri != nullptr ? uri : "", sdp, length) : rtsp_server_reply_announce(server, 501);
}

int rtsp_server_connection::record_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_record
               ? handler->on_record(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale)
               : rtsp_server_reply_record(server, 501, nullptr, nullptr);
}

int rtsp_server_connection::options_callback(void*, rtsp_server_t* server, const char*)
{
    return rtsp_server_reply_options(server, 200);
}

int rtsp_server_connection::get_parameter_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const void* content, int bytes)
{
    const auto handler = static_cast<rtsp_server_connection*>(param)->handler_;
    return handler && handler->on_get_parameter
               ? handler->on_get_parameter(server, uri != nullptr ? uri : "", session != nullptr ? session : "", content, bytes)
               : rtsp_server_reply_get_parameter(server, 501, nullptr, 0);
}

void rtsp_server_connection::on_tcp_read(std::span<const std::uint8_t> data)
{
    auto remaining = data;
    while (!closed_ && !remaining.empty())
    {
        const auto handler = handler_;
        if (!handler || !handler->on_read)
        {
            shutdown();
            return;
        }

        const auto consumed = handler->on_read(remaining);
        if (closed_)
        {
            return;
        }
        if (consumed == 0 || consumed > remaining.size())
        {
            shutdown();
            return;
        }
        remaining = remaining.subspan(consumed);
    }
}


void rtsp_server_connection::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    const auto handler = std::move(handler_);
    handler_.reset();
    if (handler && handler->on_shutdown)
    {
        handler->on_shutdown();
    }
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
        server_ = nullptr;
    }
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
}

}    // namespace media_server
