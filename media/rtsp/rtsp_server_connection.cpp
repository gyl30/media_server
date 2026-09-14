#include <chrono>
#include <vector>
#include <string>
#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>

#include "media/http/signaling_client.h"
#include "media/net/worker_context.h"
#include "media/rtsp/rtsp_publish_session.h"
#include "media/rtsp/rtsp_play_session.h"
#include "media/rtsp/rtsp_server_connection.h"

extern "C"
{
#include "rtp-over-rtsp.h"
}

namespace media_server
{
rtsp_server_connection::rtsp_server_connection(worker_context& worker,
                                               boost::asio::ip::tcp::socket socket,
                                               video_transcode_codec video_codec,
                                               std::shared_ptr<signaling_client> signaling,
                                               std::chrono::milliseconds inactivity_timeout,
                                               std::size_t max_write_queue_bytes)
    : worker_(worker),
      video_codec_(video_codec),
      signaling_(std::move(signaling)),
      transport_(std::move(socket)),
      inactivity_timer_(worker_.io()),
      inactivity_timeout_(inactivity_timeout),
      max_write_queue_bytes_(max_write_queue_bytes)
{
}

rtsp_server_connection::~rtsp_server_connection() = default;

void rtsp_server_connection::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(),
                       [self](boost::asio::yield_context yield) { self->run(yield); },
                       boost::asio::bind_cancellation_slot(run_cancellation_.slot(), boost::asio::detached));
}

void rtsp_server_connection::run(boost::asio::yield_context yield)
{
    yield.throw_if_cancelled(false);
    if (closed_)
    {
        return;
    }
    boost::system::error_code endpoint_error;
    const auto peer = transport_.remote_endpoint(endpoint_error);
    if (endpoint_error)
    {
        shutdown();
        return;
    }
    const auto local = transport_.local_endpoint(endpoint_error);
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
    local_address_ = local.address();
    record_control_activity();
    schedule_inactivity_timeout();

    rtp_over_rtsp_t interleaved{};
    interleaved.onrtp = &rtsp_server_connection::interleaved_callback;
    interleaved.param = this;
    bool rtsp_need_more_data{};
    std::vector<std::uint8_t> buffer(64 * 1024);

    bool stop = false;
    while (!stop)
    {
        boost::system::error_code error;
        const auto bytes = transport_.read(buffer, yield, error);
        if (error || closed_)
        {
            break;
        }

        auto remaining = std::span{buffer.data(), bytes};
        while (!remaining.empty())
        {
            std::size_t consumed{};
            if (!rtsp_need_more_data && (interleaved.state != 0 || remaining.front() == '$'))
            {
                if (!publish_session_ && !play_session_)
                {
                    stop = true;
                    break;
                }

                const auto* next = rtp_over_rtsp(&interleaved, remaining.data(), remaining.data() + remaining.size());
                consumed = static_cast<std::size_t>(next - remaining.data());
            }
            else
            {
                auto remaining_bytes = remaining.size();
                const auto result = rtsp_server_input(rtsp_context, remaining.data(), &remaining_bytes);
                rtsp_need_more_data = result > 0;
                if (result < 0)
                {
                    stop = true;
                    break;
                }
                consumed = remaining.size() - remaining_bytes;
                if (result == 0 && consumed == 0)
                {
                    stop = true;
                    break;
                }
            }

            if (consumed == 0 || consumed > remaining.size())
            {
                stop = true;
                break;
            }
            remaining = remaining.subspan(consumed);

            if (publish_claim_pending_ && !run_publish_claim(rtsp_context, yield))
            {
                stop = true;
                break;
            }
            if (closed_ || closing_after_write_)
            {
                stop = true;
                break;
            }
        }
    }

    rtsp_server_destroy(rtsp_context);
    if (interleaved.data != nullptr)
    {
        std::free(interleaved.data);
    }
    if (!closing_after_write_)
    {
        shutdown();
    }
}

bool rtsp_server_connection::run_publish_claim(rtsp_server_t* server, boost::asio::yield_context& yield)
{
    const auto publish = publish_session_;
    const auto stream_id = publish->stream_id();
    const auto stream_name = publish->stream_name();
    const auto result = signaling_->claim_publish(stream_id, "rtsp", stream_name, yield);
    if (closed_ || !publish_claim_pending_ || publish_session_ != publish)
    {
        return false;
    }

    publish_claim_pending_ = false;
    if (result.kind == signaling_result_kind::accepted)
    {
        record_control_activity();
        if (publish->accept_announce(server) != 0)
        {
            shutdown();
            return false;
        }
        return true;
    }

    spdlog::warn("rtsp publish claim failed stream {} stream_id {} status {} error {}", stream_name, stream_id, result.status, result.error);
    publish->shutdown();
    publish_session_.reset();
    const auto status = result.kind == signaling_result_kind::rejected ? 403 : 503;
    static_cast<void>(reply_announce_and_close(server, status));
    return false;
}

void rtsp_server_connection::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

int rtsp_server_connection::send_callback(void* param, const void* data, std::size_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->write(std::span{static_cast<const std::uint8_t*>(data), bytes});
    return 0;
}

void rtsp_server_connection::interleaved_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->publish_session_)
    {
        if (!self->publish_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes)))
        {
            self->shutdown();
        }
        return;
    }
    if (self->play_session_)
    {
        if (!self->play_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes)))
        {
            self->shutdown();
        }
        return;
    }
    self->shutdown();
}

int rtsp_server_connection::describe_callback(void* param, rtsp_server_t* server, const char* uri)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->publish_session_)
    {
        return rtsp_server_reply_describe(server, 501, "");
    }
    if (!self->play_session_)
    {
        const auto owner = self->shared_from_this();
        auto next_session = std::make_shared<rtsp_play_session>(self->worker_,
                                                                self->video_codec_,
                                                                self->local_address_,
                                                                [owner](std::span<const std::uint8_t> data) { owner->write(data); });
        next_session->set_shutdown_handler([owner]() { owner->shutdown(); });
        self->play_session_ = std::move(next_session);
    }
    return self->play_session_->on_describe(server, uri != nullptr ? uri : "");
}

int rtsp_server_connection::setup_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->publish_session_)
    {
        return self->publish_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
    }
    if (!self->play_session_)
    {
        const auto owner = self->shared_from_this();
        auto next_session = std::make_shared<rtsp_play_session>(self->worker_,
                                                                self->video_codec_,
                                                                self->local_address_,
                                                                [owner](std::span<const std::uint8_t> data) { owner->write(data); });
        next_session->set_shutdown_handler([owner]() { owner->shutdown(); });
        self->play_session_ = std::move(next_session);
    }
    return self->play_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int rtsp_server_connection::play_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->play_session_)
    {
        return self->play_session_->on_play(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
    }
    if (self->publish_session_)
    {
        return rtsp_server_reply_play(server, 501, nullptr, nullptr, nullptr);
    }
    return -1;
}

int rtsp_server_connection::teardown_callback(void* param, rtsp_server_t* server, const char* uri, const char* session)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->publish_session_)
    {
        return self->publish_session_->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
    }
    if (self->play_session_)
    {
        return self->play_session_->on_teardown(server, uri != nullptr ? uri : "", session != nullptr ? session : "");
    }
    return -1;
}

int rtsp_server_connection::announce_callback(void* param, rtsp_server_t* server, const char* uri, const char* sdp, int length)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->play_session_)
    {
        return rtsp_server_reply_announce(server, 501);
    }
    if (self->publish_session_)
    {
        return rtsp_server_reply_announce(server, 455);
    }
    if (!self->signaling_)
    {
        return self->reply_announce_and_close(server, 503);
    }

    const auto owner = self->shared_from_this();
    auto next_session = std::make_shared<rtsp_publish_session>(
        self->worker_, self->local_address_, [owner](std::span<const std::uint8_t> data) { owner->write(data); });
    next_session->set_shutdown_handler([owner]() { owner->shutdown(); });
    const auto status = next_session->prepare_announce(server, uri != nullptr ? uri : "", sdp, length);
    if (status != 200)
    {
        next_session->shutdown();
        return rtsp_server_reply_announce(server, status);
    }

    self->publish_session_ = std::move(next_session);
    self->publish_claim_pending_ = true;
    return 0;
}

int rtsp_server_connection::record_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (self->publish_session_)
    {
        return self->publish_session_->on_record(server, uri != nullptr ? uri : "", session != nullptr ? session : "", npt, scale);
    }
    if (self->play_session_)
    {
        return rtsp_server_reply_record(server, 501, nullptr, nullptr);
    }
    return -1;
}

int rtsp_server_connection::options_callback(void* param, rtsp_server_t* server, const char*)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    return rtsp_server_reply_options(server, 200);
}

int rtsp_server_connection::get_parameter_callback(void* param, rtsp_server_t* server, const char*, const char* session, const void*, int bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    self->record_control_activity();
    if (!self->publish_session_ && !self->play_session_ && (bytes != 0 || (session != nullptr && session[0] != '\0')))
    {
        return -1;
    }
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

void rtsp_server_connection::write(std::span<const std::uint8_t> data)
{
    const bool close_after_write = std::exchange(close_next_write_, false);
    if (data.empty())
    {
        if (close_after_write)
        {
            shutdown();
        }
        return;
    }

    if (data.size() > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - data.size())
    {
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back({
        .data = std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()),
        .close_after_write = close_after_write,
    });
    queued_write_bytes_ += data.size();
    if (close_after_write)
    {
        closing_after_write_ = true;
    }
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_write(yield); }, boost::asio::detached);
    }
}

int rtsp_server_connection::reply_announce_and_close(rtsp_server_t* server, int status)
{
    close_next_write_ = true;
    const auto result = rtsp_server_reply_announce(server, status);
    close_next_write_ = false;
    if (!closing_after_write_)
    {
        shutdown();
    }
    return result;
}

void rtsp_server_connection::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto entry = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(transport_.write(*entry.data, yield, error));
        if (error || closed_)
        {
            shutdown();
            return;
        }

        queued_write_bytes_ -= entry.data->size();
        write_queue_.pop_front();
        if (entry.close_after_write)
        {
            shutdown();
            return;
        }
    }
}

void rtsp_server_connection::record_control_activity() { last_control_activity_ = std::chrono::steady_clock::now(); }

void rtsp_server_connection::schedule_inactivity_timeout()
{
    inactivity_timer_.expires_at(last_control_activity_ + inactivity_timeout_);
    const auto self = shared_from_this();
    inactivity_timer_.async_wait(
        [self](const boost::system::error_code& error)
        {
            if (error || self->closed_)
            {
                return;
            }
            if (std::chrono::steady_clock::now() < self->last_control_activity_ + self->inactivity_timeout_)
            {
                self->schedule_inactivity_timeout();
                return;
            }
            self->shutdown();
        });
}

void rtsp_server_connection::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    run_cancellation_.emit(boost::asio::cancellation_type::all);
    inactivity_timer_.cancel();
    if (publish_session_)
    {
        publish_session_->shutdown();
        publish_session_.reset();
    }
    if (play_session_)
    {
        play_session_->shutdown();
        play_session_.reset();
    }
    transport_.shutdown();
}

}    // namespace media_server
