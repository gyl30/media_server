#include <chrono>
#include <string>
#include <vector>
#include <cstdlib>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/asio/detached.hpp>
#include <boost/scope/scope_exit.hpp>

#include "media/rtsp/rtsp_uri.h"
#include "media/rtsp/rtsp_event.h"
#include "media/net/worker_context.h"
#include "media/http/signaling_client.h"
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

rtsp_server_connection::rtsp_server_connection(worker_context& worker,
                                               boost::asio::ip::tcp::socket socket,
                                               video_transcode_codec video_codec,
                                               std::chrono::milliseconds inactivity_timeout,
                                               std::size_t max_write_queue_bytes)
    : worker_(worker),
      video_codec_(video_codec),
      transport_(std::move(socket)),
      write_queue_(max_write_queue_bytes),
      inactivity_timer_(worker_.io()),
      inactivity_timeout_(inactivity_timeout)
{
}

void rtsp_server_connection::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void rtsp_server_connection::run(boost::asio::yield_context yield)
{
    if (closed_)
    {
        return;
    }

    yield_ = &yield;
    boost::scope::scope_exit clear_yield([this]() { yield_ = nullptr; });
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
    boost::scope::scope_exit destroy_context([rtsp_context]() { rtsp_server_destroy(rtsp_context); });
    local_address_ = local.address();
    record_control_activity();
    schedule_inactivity_timeout();

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
        const auto bytes = transport_.read(buffer, yield, error);
        if (error)
        {
            report_transport_error(error);
            shutdown();
            return;
        }
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
                const auto result = rtsp_server_input(rtsp_context, remaining.data(), &remaining_bytes);
                rtsp_need_more_data = result > 0;
                if (result < 0)
                {
                    report_publisher_event(event_state::protocol_error, "control", "rtsp_input_failed");
                    report_output_event(event_state::protocol_error, "control", "rtsp_input_failed");
                    shutdown();
                    return;
                }
                consumed = remaining.size() - remaining_bytes;
                if (result == 0 && consumed == 0)
                {
                    report_publisher_event(event_state::protocol_error, "control", "rtsp_input_made_no_progress");
                    report_output_event(event_state::protocol_error, "control", "rtsp_input_made_no_progress");
                    shutdown();
                    return;
                }
            }

            if (consumed == 0 || consumed > remaining.size())
            {
                report_publisher_event(event_state::protocol_error, "control", "invalid_rtsp_input_consumption");
                report_output_event(event_state::protocol_error, "control", "invalid_rtsp_input_consumption");
                shutdown();
                return;
            }
            remaining = remaining.subspan(consumed);

            if (closed_ || closing_after_write_)
            {
                return;
            }
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
    self->write(std::span{static_cast<const std::uint8_t*>(data), bytes});
    return 0;
}

void rtsp_server_connection::interleaved_callback(void* param, std::uint8_t channel, const void* data, std::uint16_t bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_)
    {
        return;
    }
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
    if (self->closed_)
    {
        return -1;
    }
    self->record_control_activity();
    if (self->publish_session_)
    {
        return rtsp_server_reply_describe(server, 501, "");
    }
    if (!self->play_session_)
    {
        const auto status = self->admit_play(uri != nullptr ? uri : "", false);
        if (status < 0)
        {
            return -1;
        }
        if (status != 200)
        {
            self->close_next_write_ = true;
            const auto result = rtsp_server_reply_describe(server, status, "");
            self->close_next_write_ = false;
            if (!self->closing_after_write_)
            {
                self->shutdown();
            }
            return result;
        }
    }
    return self->play_session_->on_describe(server, uri != nullptr ? uri : "");
}

int rtsp_server_connection::setup_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_)
    {
        return -1;
    }
    self->record_control_activity();
    if (self->publish_session_)
    {
        return self->publish_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
    }
    if (!self->play_session_)
    {
        const auto status = self->admit_play(uri != nullptr ? uri : "", true);
        if (status < 0)
        {
            return -1;
        }
        if (status != 200)
        {
            self->close_next_write_ = true;
            const auto result = rtsp_server_reply_setup(server, status, nullptr, nullptr);
            self->close_next_write_ = false;
            if (!self->closing_after_write_)
            {
                self->shutdown();
            }
            return result;
        }
    }
    return self->play_session_->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
}

int rtsp_server_connection::play_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_)
    {
        return -1;
    }
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
    if (self->closed_)
    {
        return -1;
    }
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
    if (self->closed_)
    {
        return -1;
    }
    self->record_control_activity();
    if (self->play_session_)
    {
        return rtsp_server_reply_announce(server, 501);
    }
    if (self->publish_session_)
    {
        return rtsp_server_reply_announce(server, 455);
    }
    const auto owner = self->shared_from_this();
    auto publish = std::make_shared<rtsp_publish_session>(
        self->worker_, self->local_address_, [owner](std::span<const std::uint8_t> data) { owner->write(data); });
    publish->set_shutdown_handler([owner]() { owner->shutdown(); });
    const auto status = publish->prepare_announce(server, uri != nullptr ? uri : "", sdp, length);
    if (status != 200)
    {
        publish->shutdown();
        return rtsp_server_reply_announce(server, status);
    }

    self->publish_session_ = publish;
    const auto stream_id = publish->stream_id();
    const auto stream_name = publish->stream_name();
    const auto result = signaling_client::instance().claim_publish(stream_id, "rtsp", stream_name, *self->yield_);
    if (self->closed_ || self->publish_session_ != publish)
    {
        return -1;
    }

    if (result.kind == signaling_result_kind::accepted)
    {
        self->record_control_activity();
        const auto reply_result = publish->accept_announce(server);
        if (reply_result != 0)
        {
            self->shutdown();
        }
        return reply_result;
    }

    spdlog::warn("rtsp publish claim failed stream {} stream_id {} status {} error {}", stream_name, stream_id, result.status, result.error);
    publish->shutdown();
    self->publish_session_.reset();
    const auto reply_status = result.kind == signaling_result_kind::rejected ? 403 : 503;
    return self->reply_announce_and_close(server, reply_status);
}

int rtsp_server_connection::record_callback(
    void* param, rtsp_server_t* server, const char* uri, const char* session, const std::int64_t* npt, const double* scale)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_)
    {
        return -1;
    }
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
    if (self->closed_)
    {
        return -1;
    }
    self->record_control_activity();
    return rtsp_server_reply_options(server, 200);
}

int rtsp_server_connection::get_parameter_callback(void* param, rtsp_server_t* server, const char*, const char* session, const void*, int bytes)
{
    auto* self = static_cast<rtsp_server_connection*>(param);
    if (self->closed_)
    {
        return -1;
    }
    self->record_control_activity();
    if (!self->publish_session_ && !self->play_session_ && (bytes != 0 || (session != nullptr && session[0] != '\0')))
    {
        return -1;
    }
    return rtsp_server_reply_get_parameter(server, 200, nullptr, 0);
}

void rtsp_server_connection::write(std::span<const std::uint8_t> data)
{
    if (closed_)
    {
        return;
    }
    const bool close_after_write = std::exchange(close_next_write_, false);
    if (data.empty())
    {
        if (close_after_write)
        {
            shutdown();
        }
        return;
    }

    const auto result = write_queue_.enqueue(std::make_shared<std::vector<std::uint8_t>>(data.begin(), data.end()), close_after_write);
    if (result == tcp_write_enqueue_result::overflow)
    {
        report_publisher_event(event_state::runtime_error, "transport", "write_queue_overflow");
        report_output_event(event_state::runtime_error, "transport", "write_queue_overflow");
        shutdown();
        return;
    }
    if (result == tcp_write_enqueue_result::stopped)
    {
        return;
    }

    if (close_after_write)
    {
        closing_after_write_ = true;
    }
    if (result == tcp_write_enqueue_result::start_writer)
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
        if (closed_ || write_queue_.stopped())
        {
            return;
        }
        if (write_queue_.empty())
        {
            return;
        }

        const auto result = write_queue_.write_one(transport_, yield);
        if (result.error)
        {
            report_transport_error(result.error);
            shutdown();
            return;
        }
        if (result.stop_after_write)
        {
            shutdown();
            return;
        }
    }
}

int rtsp_server_connection::admit_play(std::string_view uri, bool track_uri)
{
    auto target = parse_rtsp_target(uri);
    if (!target)
    {
        return 400;
    }
    if (track_uri)
    {
        const auto separator = target->stream_name.rfind('/');
        if (separator == std::string::npos || separator == 0)
        {
            return 400;
        }
        target->stream_name.resize(separator);
    }

    const auto result = signaling_client::instance().claim_play(target->stream_id, "rtsp", target->stream_name, *yield_);
    if (closed_ || play_session_ || publish_session_)
    {
        return -1;
    }
    if (result.kind != signaling_result_kind::accepted)
    {
        spdlog::warn(
            "rtsp play claim failed stream {} stream_id {} status {} error {}", target->stream_name, target->stream_id, result.status, result.error);
        return result.kind == signaling_result_kind::rejected ? 403 : 503;
    }

    const auto owner = shared_from_this();
    play_session_ = std::make_shared<rtsp_play_session>(worker_,
                                                        std::move(target->stream_id),
                                                        std::move(target->stream_name),
                                                        video_codec_,
                                                        local_address_,
                                                        [owner](std::span<const std::uint8_t> data) { owner->write(data); });
    play_session_->set_shutdown_handler([owner]() { owner->shutdown(); });
    play_session_->startup();
    return 200;
}

void rtsp_server_connection::report_publisher_event(event_state state, std::string_view stage, std::string_view error)
{
    if (!publish_session_)
    {
        return;
    }
    rtsp_event::report_publisher(state, publish_session_->stream_id(), publish_session_->stream_name(), stage, error);
}

void rtsp_server_connection::report_output_event(event_state state, std::string_view stage, std::string_view error)
{
    if (!play_session_)
    {
        return;
    }
    rtsp_event::report_output(state, play_session_->stream_id(), play_session_->stream_name(), stage, error);
}

void rtsp_server_connection::report_transport_error(const boost::system::error_code& error)
{
    report_publisher_event(event_state::runtime_error, "transport", error.message());
    report_output_event(event_state::runtime_error, "transport", error.message());
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
            self->report_publisher_event(event_state::timeout, "control", "inactivity_timeout");
            self->report_output_event(event_state::timeout, "control", "inactivity_timeout");
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
    write_queue_.stop();
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
