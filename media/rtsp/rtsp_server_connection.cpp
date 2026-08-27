#include <string>
#include <cstdlib>
#include <utility>

#include <boost/asio/post.hpp>

#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server_connection::rtsp_server_connection(std::shared_ptr<tcp_connection> connection, const config& config)
    : executor_(connection->socket().get_executor()), config_(config), connection_(std::move(connection))
{
    interleaved_.onrtp = &rtsp_server_connection::interleaved_callback;
    interleaved_.param = this;
}

rtsp_server_connection::~rtsp_server_connection() = default;

void rtsp_server_connection::startup()
{
    boost::system::error_code endpoint_error;
    const auto peer = connection_->socket().remote_endpoint(endpoint_error);
    if (endpoint_error)
    {
        shutdown();
        return;
    }
    const auto local = connection_->socket().local_endpoint(endpoint_error);
    if (endpoint_error)
    {
        shutdown();
        return;
    }

    rtsp_handler_t rtsp_handler{};
    rtsp_handler.send = &rtsp_server_connection::send_callback;
    rtsp_handler.ondescribe = &rtsp_server_connection::describe_callback;
    rtsp_handler.onsetup = &rtsp_server_connection::setup_callback;
    rtsp_handler.onplay = &rtsp_server_connection::play_callback;
    rtsp_handler.onteardown = &rtsp_server_connection::teardown_callback;
    rtsp_handler.onannounce = &rtsp_server_connection::announce_callback;
    rtsp_handler.onrecord = &rtsp_server_connection::record_callback;
    rtsp_handler.onoptions = &rtsp_server_connection::options_callback;
    rtsp_handler.ongetparameter = &rtsp_server_connection::get_parameter_callback;

    const auto peer_address = peer.address().to_string();
    server_ = rtsp_server_create(peer_address.c_str(), peer.port(), &rtsp_handler, this, this);
    if (server_ == nullptr)
    {
        shutdown();
        return;
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
}

void rtsp_server_connection::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(executor_, [self]() { self->safe_shutdown(); });
}

int rtsp_server_connection::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->connection_->write(data, bytes);
    return 0;
}

void rtsp_server_connection::interleaved_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        self->shutdown();
        return;
    }
    self->session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes));
}

int rtsp_server_connection::describe_callback(void* param, rtsp_server_t* server, const char* uri)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        const auto connection = self->connection_;
        auto next_session = std::make_unique<rtsp_output_session>(
            self->executor_, self->config_, self->local_address_, [connection](std::span<const std::uint8_t> data) { connection->write(data); });
        next_session->set_error_handle([owner = self->shared_from_this()](boost::system::error_code) { owner->shutdown(); });
        self->session_ = std::move(next_session);
    }
    return self->session_->on_describe(server, uri != nullptr ? uri : "");
}

int rtsp_server_connection::setup_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        const auto connection = self->connection_;
        auto next_session = std::make_unique<rtsp_output_session>(
            self->executor_, self->config_, self->local_address_, [connection](std::span<const std::uint8_t> data) { connection->write(data); });
        next_session->set_error_handle([owner = self->shared_from_this()](boost::system::error_code) { owner->shutdown(); });
        self->session_ = std::move(next_session);
    }
    return self->session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int rtsp_server_connection::play_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        return -1;
    }
    return self->session_->on_play(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
}

int rtsp_server_connection::teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        return -1;
    }
    return self->session_->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
}

int rtsp_server_connection::announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        const auto connection = self->connection_;
        auto next_session = std::make_unique<rtsp_input_session>(
            self->executor_, [connection](std::span<const std::uint8_t> data) { connection->write(data); });
        next_session->set_error_handle([owner = self->shared_from_this()](boost::system::error_code) { owner->shutdown(); });
        self->session_ = std::move(next_session);
    }
    return self->session_->on_announce(server, uri != nullptr ? uri : "", sdp, length);
}

int rtsp_server_connection::record_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        return -1;
    }
    return self->session_->on_record(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
}

int rtsp_server_connection::options_callback(void* param, rtsp_server_t* server, const char* uri)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    return self->session_ ? self->session_->on_options(server, uri != nullptr ? uri : "") : rtsp_server_reply_options(server, 200);
}

int rtsp_server_connection::get_parameter_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const void* content, int bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->session_)
    {
        return bytes == 0 && (session == nullptr || session[0] == '\0') ? rtsp_server_reply_get_parameter(server, 200, nullptr, 0) : -1;
    }
    return self->session_->on_get_parameter(
        server, uri != nullptr ? uri : "", session != nullptr ? session : "", content, bytes);
}

void rtsp_server_connection::on_tcp_read(std::span<const std::uint8_t> data)
{
    auto remaining = data;
    while (!remaining.empty())
    {
        std::size_t consumed{};
        if (!rtsp_need_more_data_ && (interleaved_.state != 0 || remaining.front() == '$'))
        {
            if (!session_)
            {
                shutdown();
                return;
            }

            const auto* next = rtp_over_rtsp(&interleaved_, remaining.data(), remaining.data() + remaining.size());
            consumed = static_cast<std::size_t>(next - remaining.data());
        }
        else
        {
            auto bytes = remaining.size();
            const auto result = rtsp_server_input(server_, remaining.data(), &bytes);
            rtsp_need_more_data_ = result > 0;
            if (result < 0)
            {
                shutdown();
                return;
            }
            consumed = remaining.size() - bytes;
            if (result == 0 && consumed == 0)
            {
                shutdown();
                return;
            }
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

    const auto session = std::move(session_);
    session_.reset();
    if (session)
    {
        session->shutdown();
    }
    if (server_ != nullptr)
    {
        rtsp_server_destroy(server_);
        server_ = nullptr;
    }
    if (interleaved_.data != nullptr)
    {
        std::free(interleaved_.data);
        interleaved_.data = nullptr;
    }
    if (connection_)
    {
        connection_->shutdown();
        connection_.reset();
    }
}

}    // namespace media_server
