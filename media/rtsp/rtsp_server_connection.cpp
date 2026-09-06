#include <chrono>
#include <vector>
#include <string>
#include <cstdlib>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>

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
                                                   output_video_codec video_codec,
                                                   std::chrono::milliseconds inactivity_timeout,
                                                   std::size_t max_write_queue_bytes)
    : worker_(worker),
      video_codec_(video_codec),
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
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void rtsp_server_connection::run(boost::asio::yield_context yield)
{
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
        if (error)
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
        }
    }

    const auto publish_session = std::move(publish_session_);
    const auto play_session = std::move(play_session_);
    if (publish_session)
    {
        publish_session->shutdown();
    }
    if (play_session)
    {
        play_session->shutdown();
    }
    rtsp_server_destroy(rtsp_context);
    if (interleaved.data != nullptr)
    {
        std::free(interleaved.data);
    }
    shutdown();
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
        self->publish_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes));
        return;
    }
    if (self->play_session_)
    {
        self->play_session_->on_interleaved(channel, std::span(static_cast<const std::uint8_t*>(data), bytes));
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
        next_session->set_error_handler([owner](boost::system::error_code) { owner->shutdown(); });
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
        next_session->set_error_handler([owner](boost::system::error_code) { owner->shutdown(); });
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
    if (!self->publish_session_)
    {
        const auto owner = self->shared_from_this();
        auto next_session = std::make_shared<rtsp_publish_session>(
            self->worker_, self->local_address_, [owner](std::span<const std::uint8_t> data) { owner->write(data); });
        next_session->set_error_handler([owner](boost::system::error_code) { owner->shutdown(); });
        self->publish_session_ = std::move(next_session);
    }
    return self->publish_session_->on_announce(server, uri != nullptr ? uri : "", sdp, length);
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
    if (data.empty())
    {
        return;
    }

    if (data.size() > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - data.size())
    {
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    write_queue_.push_back(std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()));
    queued_write_bytes_ += data.size();
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_write(yield); }, boost::asio::detached);
    }
}

void rtsp_server_connection::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(transport_.write(*data, yield, error));
        if (error)
        {
            shutdown();
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
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
    inactivity_timer_.cancel();
    transport_.shutdown();
}

}    // namespace media_server
