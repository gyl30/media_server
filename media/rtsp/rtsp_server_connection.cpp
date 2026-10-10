#include <string>
#include <vector>
#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/rtsp/rtsp_uri.h"
#include "config.h"
#include "media/http/signaling_verify.h"
#include "media/core/stream_registry.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_play_session.h"
#include "media/rtsp/rtsp_publish_session.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "rtp-over-rtsp.h"
}

namespace media_server
{
namespace
{
constexpr std::size_t rtsp_read_buffer_bytes = 64U * 1024U;
}    // namespace

rtsp_server_connection::rtsp_server_connection(worker_context& worker, boost::asio::ip::tcp::socket socket, const config& application_config)
    : worker_(worker), config_(application_config), transport_(std::make_shared<tcp_transport>(std::move(socket))), idle_timer_(worker.io())
{
}

void rtsp_server_connection::startup()
{
    const auto self = shared_from_this();
    worker_.spawn([self](boost::asio::yield_context yield) { self->run(yield); });
}

void rtsp_server_connection::run(boost::asio::yield_context yield)
{
    const auto self = shared_from_this();
    transport_->set_write_callback(
        [self](boost::system::error_code error, std::size_t)
        {
            if (error)
            {
                spdlog::debug("rtsp write failed: {}", error.message());
                self->shutdown();
            }
        });
    // 收到数据即刷新（UDP 推流由 UDP 会话收到 RTP 时刷新）；播放连接停止计时。
    idle_timer_.start(self,
                      [this]()
                      {
                          spdlog::info("rtsp input idle timeout");
                          shutdown();
                      });
    boost::system::error_code endpoint_error;
    const auto peer = transport_->remote_endpoint(endpoint_error);
    if (endpoint_error)
    {
        shutdown();
        return;
    }
    const auto local = transport_->local_endpoint(endpoint_error);
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
    auto* rtsp_context = rtsp_server_create(peer_address.c_str(), peer.port(), &rtsp_handler, this, this);
    if (rtsp_context == nullptr)
    {
        shutdown();
        return;
    }
    boost::scope::scope_exit destroy_context([rtsp_context]() { rtsp_server_destroy(rtsp_context); });
    local_address_ = local.address();
    run_read(rtsp_context, yield);
}

void rtsp_server_connection::run_read(rtsp_server_t* server, boost::asio::yield_context yield)
{
    const auto transport = transport_;
    input_yield_ = &yield;
    boost::scope::scope_exit clear_yield([this]() { input_yield_ = nullptr; });
    rtp_over_rtsp_t interleaved{};
    interleaved.onrtp = &rtsp_server_connection::interleaved_callback;
    interleaved.param = this;
    boost::scope::scope_exit destroy_interleaved(
        [&interleaved]()
        {
            if (interleaved.data != nullptr)
            {
                std::free(interleaved.data);
            }
        });
    bool rtsp_need_more_data{};
    std::vector<std::uint8_t> buffer(rtsp_read_buffer_bytes);

    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport->read(buffer, yield, error);
        if (error)
        {
            if (yield.cancelled() == boost::asio::cancellation_type::none)
            {
                spdlog::debug("rtsp read failed: {}", error.message());
            }
            shutdown();
            return;
        }
        idle_timer_.touch();
        auto remaining = std::span{buffer.data(), bytes};

        while (!remaining.empty())
        {
            std::size_t consumed{};
            if (!rtsp_need_more_data && (interleaved.state != 0 || remaining.front() == '$'))
            {
                if (!publish_session_ && !play_session_)
                {
                    shutdown();
                    return;
                }

                const auto* next = rtp_over_rtsp(&interleaved, remaining.data(), remaining.data() + remaining.size());
                consumed = static_cast<std::size_t>(next - remaining.data());
            }
            else
            {
                auto remaining_bytes = remaining.size();
                const auto result = rtsp_server_input(server, remaining.data(), &remaining_bytes);
                rtsp_need_more_data = result > 0;
                if (result < 0)
                {
                    shutdown();
                    return;
                }
                consumed = remaining.size() - remaining_bytes;
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
}

void rtsp_server_connection::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

int rtsp_server_connection::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->transport_)
    {
        return -1;
    }
    self->transport_->write(std::span{static_cast<const std::uint8_t*>(data), bytes});
    return 0;
}

void rtsp_server_connection::interleaved_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->publish_session_)
    {
        if (!self->publish_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes)))
        {
            spdlog::debug("rtsp publish interleaved input failed channel {}", channel);
            self->shutdown();
        }
        return;
    }
    if (self->play_session_)
    {
        if (!self->play_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes)))
        {
            spdlog::debug("rtsp play interleaved input failed channel {}", channel);
            self->shutdown();
        }
        return;
    }
    spdlog::debug("rtsp interleaved input without active session channel {}", channel);
    self->shutdown();
}

int rtsp_server_connection::describe_callback(void* param, rtsp_server_t* server, const char* uri)
{
    try
    {
        auto* self = static_cast<rtsp_server_connection*>(param);
        if (self->publish_session_)
        {
            spdlog::debug("rtsp describe rejected while publishing");
            return -1;
        }
        if (!self->play_session_ && !self->admit_play(uri != nullptr ? uri : ""))
        {
            spdlog::debug("rtsp describe invalid target: {}", uri != nullptr ? uri : "");
            return -1;
        }
        return self->play_session_->on_describe(server, uri != nullptr ? uri : "");
    }
    catch (const std::exception& error)
    {
        spdlog::error("rtsp describe failed: {}", error.what());
        return -1;
    }
}

int rtsp_server_connection::setup_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->publish_session_)
    {
        return self->publish_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
    }
    if (!self->play_session_)
    {
        spdlog::debug("rtsp setup invalid target: {}", uri != nullptr ? uri : "");
        return -1;
    }
    return self->play_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int rtsp_server_connection::play_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->play_session_)
    {
        return self->play_session_->on_play(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
    }
    spdlog::debug("rtsp play rejected without play session");
    return -1;
}

int rtsp_server_connection::teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->publish_session_)
    {
        return self->publish_session_->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
    }
    if (self->play_session_)
    {
        return self->play_session_->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
    }
    spdlog::debug("rtsp teardown rejected without active session");
    return -1;
}

int rtsp_server_connection::announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length)
{
    try
    {
        auto* self = static_cast<rtsp_server_connection*>(param);
        if (self->play_session_ || self->publish_session_)
        {
            spdlog::debug("rtsp announce rejected with active session");
            return -1;
        }
        const auto target = parse_rtsp_target(uri != nullptr ? uri : "");
        if (!target || !valid_stream_token(target->stream_id))
        {
            return -1;
        }
        const auto owner = self->shared_from_this();
        auto publish = std::make_shared<rtsp_publish_session>(
            self->worker_,
            target->stream_id,
            self->local_address_,
            [owner](std::span<const std::uint8_t> data)
            {
                if (owner->transport_)
                {
                    owner->transport_->write(data);
                }
            },
            [owner]()
            {
                owner->idle_timer_.touch();
            });
        publish->set_shutdown_handler([owner]() { owner->shutdown(); });
        if (!publish->on_announce(server, uri != nullptr ? uri : "", sdp, length))
        {
            spdlog::debug("rtsp announce rejected");
            publish->shutdown();
            return -1;
        }
        if (!verify_stream(self->config_, target->stream_id, "publish", target->stream_id, *self->input_yield_) || !self->transport_)
        {
            publish->shutdown();
            return -1;
        }
        self->publish_session_ = publish;
        if (!session_registry::instance().add_receiver_session(target->stream_id, owner))
        {
            return -1;
        }
        return rtsp_server_reply_announce(server, 200);
    }
    catch (const std::exception& error)
    {
        spdlog::error("rtsp announce failed: {}", error.what());
        return -1;
    }
}

int rtsp_server_connection::record_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->publish_session_)
    {
        return self->publish_session_->on_record(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
    }
    spdlog::debug("rtsp record rejected without publish session");
    return -1;
}

int rtsp_server_connection::options_callback(void*, rtsp_server_t* server, const char*)
{
    return rtsp_server_reply_options(server, 200);
}

int rtsp_server_connection::get_parameter_callback(void* param, rtsp_server_t* server, const char*, const char* session, const void*, int bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (!self->publish_session_ && !self->play_session_ && (bytes != 0 || (session != nullptr && session[0] != '\0')))
    {
        spdlog::debug("rtsp get_parameter rejected without active session");
        return -1;
    }
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

bool rtsp_server_connection::admit_play(std::string_view uri)
{
    auto target = parse_rtsp_target(uri);
    if (!target)
    {
        return false;
    }
    const auto separator = target->stream_id.find('/');
    if (separator == std::string::npos)
    {
        return false;
    }
    const auto stream_id = std::string_view(target->stream_id).substr(0, separator);
    const auto token = std::string_view(target->stream_id).substr(separator + 1);
    if (!valid_stream_id(stream_id) || !valid_stream_token(token))
    {
        return false;
    }
    auto stream = stream_registry::instance().find(stream_id);
    if (!stream)
    {
        return false;
    }
    if (!verify_stream(config_, token, "play", stream_id, *input_yield_) || !transport_)
    {
        return false;
    }

    const auto owner = shared_from_this();
    auto player = std::make_shared<rtsp_play_session>(worker_,
                                                        std::move(stream),
                                                        target->stream_id,
                                                        local_address_,
                                                        [owner](std::vector<std::uint8_t> data)
                                                        {
                                                            if (owner->transport_)
                                                            {
                                                                owner->transport_->write(std::move(data));
                                                            }
                                                        });
    player->set_shutdown_handler([owner]() { owner->shutdown(); });
    if (player->prepare_presentation() != 0)
    {
        player->shutdown();
        return false;
    }
    play_session_ = std::move(player);
    idle_timer_.stop();
    return true;
}

void rtsp_server_connection::safe_shutdown()
{
    idle_timer_.stop();
    if (publish_session_)
    {
        session_registry::instance().remove_receiver_session(publish_session_->stream_id(), *this);
        publish_session_->shutdown();
        publish_session_.reset();
    }
    if (play_session_)
    {
        play_session_->shutdown();
        play_session_.reset();
    }
    if (const auto transport = std::exchange(transport_, {}))
    {
        transport->shutdown();
    }
}

}    // namespace media_server
