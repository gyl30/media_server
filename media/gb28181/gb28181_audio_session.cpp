#include <array>
#include <chrono>
#include <utility>
#include <unistd.h>

#include <openssl/crypto.h>
#include <boost/asio/post.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "media/gb28181/gb28181_audio_session.h"

namespace media_server
{
namespace
{
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
}    // namespace

gb28181_audio_session::gb28181_audio_session(worker_context& worker, std::string id)
    : worker_(worker), id_(std::move(id)), control_lease_(worker.io()), audio_idle_(worker.io())
{
}

std::optional<std::uint16_t> gb28181_audio_session::startup(boost::asio::ip::address bind_address)
{
    const auto self = shared_from_this();
    shutdown_subscription_ = worker_.subscribe_shutdown([self]() { self->shutdown(); });
    if (!shutdown_subscription_)
    {
        return std::nullopt;
    }
    sender_ = std::make_shared<gb28181_audio_sender>(worker_, [self](boost::system::error_code) { self->shutdown(); });
    if (!sender_->startup(std::move(bind_address)))
    {
        return std::nullopt;
    }
    control_lease_.start(self, [this]() { shutdown(); });
    audio_idle_.start(self, [this]() { shutdown(); });
    return sender_->local_port();
}

bool gb28181_audio_session::configure(boost::asio::ip::udp::endpoint rtp, boost::asio::ip::udp::endpoint rtcp, std::uint32_t ssrc, std::string token)
{
    if (!sender_ || !upload_token_.empty() || upload_ || worker_.stop_requested() ||
        session_registry::instance().find_sender_session(id_, id_).get() != this)
    {
        return false;
    }
    if (!sender_->set_peer(std::move(rtp), std::move(rtcp), ssrc))
    {
        // 同步部分失败也立即撤销认领资格；迟到配置不能再次覆盖子对象资源。
        session_registry::instance().remove_sender_session(id_, id_, *this);
        shutdown();
        return false;
    }
    upload_token_ = std::move(token);
    control_lease_.touch();
    audio_idle_.touch();
    return true;
}

void gb28181_audio_session::keep_alive()
{
    auto work = boost::asio::make_work_guard(worker_.io());
    if (worker_.stop_requested())
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self, work = std::move(work)]() { self->control_lease_.touch(); });
}

void gb28181_audio_session::attach(tcp::socket socket, http::request<http::string_body> request, std::string token)
{
    auto work = boost::asio::make_work_guard(worker_.io());
    if (worker_.stop_requested())
    {
        return;
    }
    const auto self = shared_from_this();
    boost::asio::post(
        worker_.io(),
        [self, socket = std::move(socket), request = std::move(request), token = std::move(token), work = std::move(work)]() mutable
        {
            if (self->worker_.stop_requested())
            {
                return;
            }
            boost::system::error_code error;
            const auto endpoint = socket.local_endpoint(error);
            if (error)
            {
                return;
            }
            tcp::socket rebound(self->worker_.io());
            const auto native = socket.release(error);
            if (error)
            {
                return;
            }
            boost::scope::scope_exit close_native([native]() { ::close(native); });
            rebound.assign(endpoint.protocol(), native, error);
            if (error)
            {
                return;
            }
            close_native.set_active(false);
            self->worker_.spawn([self, socket = std::move(rebound), request = std::move(request), token = std::move(token)](
                                    boost::asio::yield_context yield) mutable { self->run(std::move(socket), std::move(request), token, yield); });
        });
}

void gb28181_audio_session::run(tcp::socket socket,
                                http::request<http::string_body> request,
                                const std::string& token,
                                boost::asio::yield_context yield)
{
    boost::system::error_code error;
    if (!sender_ || upload_token_.empty() || token.size() != upload_token_.size() ||
        CRYPTO_memcmp(token.data(), upload_token_.data(), token.size()) != 0 || upload_ ||
        session_registry::instance().find_sender_session(id_, id_).get() != this)
    {
        boost::beast::tcp_stream rejected(std::move(socket));
        rejected.expires_after(std::chrono::seconds(5));
        http::response<http::empty_body> reply(http::status::forbidden, request.version());
        reply.keep_alive(false);
        reply.prepare_payload();
        http::async_write(rejected, reply, yield[error]);
        return;
    }
    // 认领后握手失败也不恢复，upload_ 同时表达该对象已有唯一上传连接。
    upload_token_.clear();
    upload_ = std::make_unique<websocket::stream<boost::beast::tcp_stream>>(std::move(socket));
    auto timeout = websocket::stream_base::timeout::suggested(boost::beast::role_type::server);
    timeout.handshake_timeout = std::chrono::seconds(5);
    timeout.idle_timeout = websocket::stream_base::none();
    timeout.keep_alive_pings = false;
    upload_->set_option(timeout);
    upload_->read_message_max(160);
    upload_->next_layer().expires_never();
    upload_->async_accept(request, yield[error]);
    if (error || !sender_ || yield.cancelled() != boost::asio::cancellation_type::none)
    {
        shutdown();
        return;
    }
    boost::beast::flat_buffer buffer(160);
    while (sender_)
    {
        upload_->async_read(buffer, yield[error]);
        if (error || !sender_ || yield.cancelled() != boost::asio::cancellation_type::none || upload_->got_text() || buffer.size() != 160)
        {
            shutdown();
            return;
        }
        std::array<std::uint8_t, 160> samples{};
        boost::asio::buffer_copy(boost::asio::buffer(samples), buffer.data());
        // UDP 丢新包不转为终止；真正发送错误由子对象错误回调关闭父会话。
        static_cast<void>(sender_->input_audio(samples));
        audio_idle_.touch();
        buffer.consume(buffer.size());
    }
}

void gb28181_audio_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void gb28181_audio_session::safe_shutdown()
{
    session_registry::instance().remove_sender_session(id_, id_, *this);
    shutdown_subscription_.reset();
    control_lease_.stop();
    audio_idle_.stop();
    upload_token_.clear();
    if (upload_)
    {
        boost::system::error_code error;
        auto& socket = upload_->next_layer().socket();
        socket.shutdown(tcp::socket::shutdown_both, error);
        socket.close(error);
    }
    if (sender_)
    {
        sender_->shutdown();
        sender_.reset();
    }
}

}    // namespace media_server
