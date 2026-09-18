#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/detached.hpp>

#include "media/core/stream_id.h"
#include "media/rtmp/rtmp_event.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/worker_context.h"
#include "media/core/stream_registry.h"
#include "media/http/signaling_client.h"
#include "media/rtmp/rtmp_play_session.h"
#include "media/rtmp/rtmp_publish_session.h"

extern "C"
{
#include "flv-proto.h"
#include "rtmp-server.h"
}

namespace media_server
{

std::optional<rtmp_target> parse_rtmp_target(std::string_view app, std::string_view stream)
{
    const auto parsed = boost::urls::parse_relative_ref(stream);
    if (!parsed || parsed->has_fragment())
    {
        return std::nullopt;
    }

    std::string path;
    for (const auto segment : parsed->segments())
    {
        if (!path.empty())
        {
            path.push_back('/');
        }
        path.append(segment);
    }
    if (path.empty())
    {
        return std::nullopt;
    }

    std::optional<std::string> stream_id;
    for (const auto parameter : parsed->params())
    {
        if (parameter.key != "stream_id")
        {
            continue;
        }
        if (stream_id || !parameter.has_value)
        {
            return std::nullopt;
        }
        stream_id = parameter.value;
    }
    if (!stream_id || !valid_stream_id(*stream_id))
    {
        return std::nullopt;
    }

    std::string stream_name;
    if (!app.empty())
    {
        stream_name.append(app);
        stream_name.push_back('/');
    }
    stream_name.append(path);
    return rtmp_target{.stream_id = std::move(*stream_id), .stream_name = std::move(stream_name)};
}

rtmp_session::rtmp_session(worker_context& worker,
                           boost::asio::ip::tcp::socket socket,
                           video_transcode_config video,
                           std::chrono::milliseconds initial_tracks_timeout,
                           std::size_t max_write_queue_bytes)
    : worker_(worker),
      transport_(std::move(socket)),
      max_write_queue_bytes_(max_write_queue_bytes),
      initial_tracks_timeout_(initial_tracks_timeout),
      video_config_(video)
{
}

void rtmp_session::startup()
{
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run(yield); }, boost::asio::detached);
}

void rtmp_session::run(boost::asio::yield_context yield)
{
    if (closed_)
    {
        return;
    }
    rtmp_server_handler_t handler{};
    handler.send = &rtmp_session::send_callback;
    handler.ondelete_stream = &rtmp_session::delete_stream_callback;
    handler.onplay = &rtmp_session::play_callback;
    handler.onpause = &rtmp_session::pause_callback;
    handler.onseek = &rtmp_session::seek_callback;
    handler.onpublish = &rtmp_session::publish_callback;
    handler.onvideo = &rtmp_session::video_callback;
    handler.onaudio = &rtmp_session::audio_callback;
    handler.onscript = &rtmp_session::script_callback;
    handler.ongetduration = &rtmp_session::duration_callback;

    auto* context = rtmp_server_create(this, &handler);
    if (context == nullptr)
    {
        shutdown();
        return;
    }
    rtmp_context_ = context;

    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport_.read(buffer, yield, error);
        if (error)
        {
            report_transport_error(error);
            break;
        }
        const auto input_result = bytes == 0 ? 0 : rtmp_server_input(context, buffer.data(), bytes);
        if (input_result != 0)
        {
            if (input_result != RTMP_SERVER_INPUT_STOP && publish_)
            {
                rtmp_event::report_publisher(
                    event_state::protocol_error, publish_->stream_id(), publish_->stream_name(), "media", "rtmp_input_failed");
            }
            else if (input_result != RTMP_SERVER_INPUT_STOP && play_)
            {
                rtmp_event::report_output(event_state::protocol_error, play_->stream_id(), play_->stream_name(), "control", "rtmp_input_failed");
            }
            if (input_result != RTMP_SERVER_INPUT_STOP)
            {
                shutdown();
            }
            break;
        }
    }

    rtmp_context_ = nullptr;
    rtmp_server_destroy(context);
    safe_shutdown();
    spdlog::debug("rtmp shutdown {}", stream_name_);
}

int rtmp_session::send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes)
{
    auto* self = static_cast<rtmp_session*>(param);
    auto data = std::make_shared<std::vector<std::uint8_t>>();
    data->reserve(header_bytes + payload_bytes);
    if (header_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(header);
        data->insert(data->end(), first, first + header_bytes);
    }
    if (payload_bytes != 0)
    {
        const auto* first = static_cast<const std::uint8_t*>(payload);
        data->insert(data->end(), first, first + payload_bytes);
    }
    self->write(std::move(data));
    return static_cast<int>(header_bytes + payload_bytes);
}

int rtmp_session::delete_stream_callback(void* param, std::uint32_t stream_id)
{
    return static_cast<rtmp_session*>(param)->on_delete_stream(stream_id);
}

int rtmp_session::play_callback(void* param, const char* app, const char* stream, double, double, std::uint8_t)
{
    return static_cast<rtmp_session*>(param)->on_play(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::pause_callback(void*, int, std::uint32_t) { return -1; }

int rtmp_session::seek_callback(void*, std::uint32_t) { return -1; }

int rtmp_session::publish_callback(void* param, const char* app, const char* stream, const char*)
{
    return static_cast<rtmp_session*>(param)->on_publish(app != nullptr ? app : "", stream != nullptr ? stream : "");
}

int rtmp_session::video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return !self->closed_ && self->publish_ ? self->publish_->on_video(data, bytes, timestamp) : -1;
}

int rtmp_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return !self->closed_ && self->publish_ ? self->publish_->on_audio(data, bytes, timestamp) : -1;
}

int rtmp_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (self->closed_ || !self->publish_)
    {
        return 0;
    }
    return self->publish_->on_script(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), bytes));
}

int rtmp_session::duration_callback(void*, const char*, const char*, double* duration)
{
    if (duration != nullptr)
    {
        *duration = 0.0;
    }
    return 0;
}

void rtmp_session::write(std::shared_ptr<std::vector<std::uint8_t>> data)
{
    if (closed_ || data->empty())
    {
        return;
    }

    if (data->size() > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - data->size())
    {
        if (publish_)
        {
            rtmp_event::report_publisher(
                event_state::runtime_error, publish_->stream_id(), publish_->stream_name(), "transport", "write_queue_overflow");
        }
        else if (play_)
        {
            rtmp_event::report_output(event_state::runtime_error, play_->stream_id(), play_->stream_name(), "transport", "write_queue_overflow");
        }
        shutdown();
        return;
    }

    const bool start_write = write_queue_.empty();
    queued_write_bytes_ += data->size();
    write_queue_.push_back(std::move(data));
    if (start_write)
    {
        const auto self = shared_from_this();
        boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_write(yield); }, boost::asio::detached);
    }
}

void rtmp_session::run_write(boost::asio::yield_context yield)
{
    for (;;)
    {
        if (closed_)
        {
            return;
        }
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        transport_.write(*data, yield, error);
        if (error)
        {
            report_transport_error(error);
            shutdown();
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

void rtmp_session::report_transport_error(const boost::system::error_code& error)
{
    if (publish_)
    {
        rtmp_event::report_publisher(event_state::runtime_error, publish_->stream_id(), publish_->stream_name(), "transport", error.message());
    }
    else if (play_)
    {
        rtmp_event::report_output(event_state::runtime_error, play_->stream_id(), play_->stream_name(), "transport", error.message());
    }
}

int rtmp_session::on_delete_stream(std::uint32_t stream_id)
{
    if (stream_id == 0)
    {
        return 0;
    }

    claim_pending_ = false;
    if (publish_)
    {
        rtmp_event::report_publisher(event_state::stop_requested, publish_->stream_id(), publish_->stream_name(), "control");
    }
    else if (play_)
    {
        rtmp_event::report_output(event_state::stop_requested, play_->stream_id(), play_->stream_name(), "control");
    }
    shutdown();
    return RTMP_SERVER_INPUT_STOP;
}

int rtmp_session::on_play(std::string app, std::string stream)
{
    if (closed_)
    {
        return -1;
    }
    if (claim_pending_)
    {
        shutdown();
        return -1;
    }
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        shutdown();
        return -1;
    }

    stream_id_ = target->stream_id;
    stream_name_ = target->stream_name;
    claim_pending_ = true;
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_play_claim(yield); }, boost::asio::detached);
    return RTMP_SERVER_ASYNC_START;
}

int rtmp_session::on_publish(std::string app, std::string stream)
{
    if (closed_)
    {
        return -1;
    }
    if (claim_pending_)
    {
        shutdown();
        return -1;
    }
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_target(app, stream);
    if (!target)
    {
        shutdown();
        return -1;
    }

    stream_id_ = target->stream_id;
    stream_name_ = target->stream_name;
    claim_pending_ = true;
    const auto self = shared_from_this();
    boost::asio::spawn(worker_.io(), [self](boost::asio::yield_context yield) { self->run_publish_claim(yield); }, boost::asio::detached);
    return RTMP_SERVER_ASYNC_START;
}

void rtmp_session::run_play_claim(boost::asio::yield_context yield)
{
    const auto result = signaling_client::instance().claim_play(stream_id_, "rtmp", stream_name_, yield);
    if (closed_ || rtmp_context_ == nullptr || !claim_pending_)
    {
        return;
    }

    claim_pending_ = false;
    if (result.kind != signaling_result_kind::accepted)
    {
        spdlog::warn("rtmp play claim failed stream {} stream_id {} status {} error {}", stream_name_, stream_id_, result.status, result.error);
        rtmp_server_start(rtmp_context_, -1, "play claim rejected");
        shutdown();
        return;
    }

    auto media = stream_registry::instance().find(stream_name_);
    if (!media)
    {
        spdlog::warn("rtmp play stream not found {}", stream_name_);
        rtmp_server_start(rtmp_context_, -1, "play stream not found");
        shutdown();
        return;
    }
    if (video_config_.codec == video_transcode_codec::av1 && !rtmp_server_peer_supports_fourcc(rtmp_context_, "av01"))
    {
        spdlog::warn("rtmp play av1 unsupported by peer {}", stream_name_);
        rtmp_server_start(rtmp_context_, -1, "play video codec unsupported");
        shutdown();
        return;
    }

    const auto self = shared_from_this();
    play_ = std::make_shared<rtmp_play_session>(
        worker_,
        stream_id_,
        stream_name_,
        std::move(media),
        [self](int type, std::span<const std::uint8_t> data, std::uint32_t timestamp)
        {
            if (self->rtmp_context_ == nullptr)
            {
                return;
            }
            if (type == FLV_TYPE_VIDEO)
            {
                static_cast<void>(rtmp_server_send_video(self->rtmp_context_, data.data(), data.size(), timestamp));
            }
            else if (type == FLV_TYPE_AUDIO)
            {
                static_cast<void>(rtmp_server_send_audio(self->rtmp_context_, data.data(), data.size(), timestamp));
            }
        },
        video_config_,
        [self]() { self->shutdown(); });

    rtmp_event::report_output(event_state::starting, play_->stream_id(), play_->stream_name(), "play");
    if (rtmp_server_start(rtmp_context_, 0, nullptr) != 0)
    {
        rtmp_event::report_output(event_state::runtime_error, play_->stream_id(), play_->stream_name(), "control", "rtmp_play_start_failed");
        shutdown();
        return;
    }
    play_->startup();
    spdlog::info("rtmp play {}", stream_name_);
}

void rtmp_session::run_publish_claim(boost::asio::yield_context yield)
{
    const auto result = signaling_client::instance().claim_publish(stream_id_, "rtmp", stream_name_, yield);
    if (closed_ || rtmp_context_ == nullptr || !claim_pending_)
    {
        return;
    }

    claim_pending_ = false;
    if (result.kind != signaling_result_kind::accepted)
    {
        spdlog::warn("rtmp publish claim failed stream {} stream_id {} status {} error {}", stream_name_, stream_id_, result.status, result.error);
        rtmp_server_start(rtmp_context_, -1, "publish claim rejected");
        shutdown();
        return;
    }

    const auto self = shared_from_this();
    auto publish = std::make_shared<rtmp_publish_session>(worker_, stream_id_, stream_name_, initial_tracks_timeout_, [self]() { self->shutdown(); });
    if (!publish->startup())
    {
        rtmp_server_start(rtmp_context_, -1, "publish startup failed");
        publish->shutdown();
        stream_id_.clear();
        shutdown();
        return;
    }

    publish_ = std::move(publish);
    if (rtmp_server_start(rtmp_context_, 0, nullptr) != 0)
    {
        rtmp_event::report_publisher(
            event_state::runtime_error, publish_->stream_id(), publish_->stream_name(), "control", "rtmp_publish_start_failed");
        shutdown();
        return;
    }
    spdlog::info("rtmp publish {}", stream_name_);
}

void rtmp_session::shutdown()
{
    const auto self = shared_from_this();
    boost::asio::post(worker_.io(), [self]() { self->safe_shutdown(); });
}

void rtmp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    claim_pending_ = false;
    rtmp_context_ = nullptr;
    if (publish_)
    {
        publish_->shutdown();
        publish_.reset();
    }
    if (play_)
    {
        play_->shutdown();
        play_.reset();
    }
    stream_id_.clear();
    transport_.shutdown();
}

}    // namespace media_server
