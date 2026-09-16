#include <vector>
#include <utility>

#include <spdlog/spdlog.h>
#include <boost/asio/post.hpp>
#include <boost/url/parse.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>

#include "media/core/stream_id.h"
#include "media/rtmp/rtmp_session.h"
#include "media/net/worker_context.h"
#include "media/http/event_reporter.h"
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

namespace
{
bool remote_disconnect(const boost::system::error_code& error)
{
    return error == boost::asio::error::eof || error == boost::asio::error::connection_reset || error == boost::asio::error::connection_aborted;
}
}    // namespace

std::optional<rtmp_publish_target> parse_rtmp_publish_target(std::string_view app, std::string_view stream)
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
    return rtmp_publish_target{.stream_id = std::move(*stream_id), .stream_name = std::move(stream_name)};
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

rtmp_session::~rtmp_session() = default;

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
        shutdown_with_stage(runtime_end_reason::runtime_error, "setup", "rtmp_server_create_failed");
        return;
    }
    rtmp_context_ = context;

    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;)
    {
        boost::system::error_code error;
        const auto bytes = transport_.read(buffer, yield, error);
        if (ending_ || closed_)
        {
            break;
        }
        if (error)
        {
            if (remote_disconnect(error))
            {
                shutdown_with_stage(runtime_end_reason::remote, "transport");
            }
            else
            {
                shutdown_with_stage(runtime_end_reason::runtime_error, "transport", error.message());
            }
            break;
        }
        if (bytes != 0 && rtmp_server_input(context, buffer.data(), bytes) != 0)
        {
            shutdown_with_stage(runtime_end_reason::protocol_error, publish_ ? "media" : "control", "rtmp_input_failed");
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
    return !self->ending_ && self->publish_ ? self->publish_->on_video(data, bytes, timestamp) : -1;
}

int rtmp_session::audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp)
{
    auto* self = static_cast<rtmp_session*>(param);
    return !self->ending_ && self->publish_ ? self->publish_->on_audio(data, bytes, timestamp) : -1;
}

int rtmp_session::script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t)
{
    auto* self = static_cast<rtmp_session*>(param);
    if (self->ending_ || !self->publish_)
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
    if (ending_ || closed_ || data->empty())
    {
        return;
    }

    if (data->size() > max_write_queue_bytes_ || queued_write_bytes_ > max_write_queue_bytes_ - data->size())
    {
        shutdown_with_stage(runtime_end_reason::runtime_error, "transport", "write_queue_overflow");
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
        if (ending_ || closed_)
        {
            return;
        }
        if (write_queue_.empty())
        {
            return;
        }

        const auto data = write_queue_.front();
        boost::system::error_code error;
        static_cast<void>(transport_.write(*data, yield, error));
        if (ending_ || closed_)
        {
            return;
        }
        if (error)
        {
            if (remote_disconnect(error))
            {
                shutdown_with_stage(runtime_end_reason::remote, "transport");
            }
            else
            {
                shutdown_with_stage(runtime_end_reason::runtime_error, "transport", error.message());
            }
            return;
        }

        queued_write_bytes_ -= data->size();
        write_queue_.pop_front();
    }
}

int rtmp_session::on_play(std::string app, std::string stream)
{
    if (ending_ || closed_)
    {
        return -1;
    }
    if (publish_claim_pending_)
    {
        shutdown_with_stage(runtime_end_reason::protocol_error, "control", "request_during_publish_claim");
        return -1;
    }
    if (publish_ || play_)
    {
        return -1;
    }

    stream_name_ = make_stream_name(app, stream);
    auto media = stream_registry::instance().find(stream_name_);
    if (!media)
    {
        spdlog::warn("rtmp play stream not found {}", stream_name_);
        return -1;
    }
    if (video_config_.codec == video_transcode_codec::av1 && !rtmp_server_peer_supports_fourcc(rtmp_context_, "av01"))
    {
        spdlog::warn("rtmp play av1 unsupported by peer {}", stream_name_);
        return -1;
    }

    const auto self = shared_from_this();
    play_ = std::make_shared<rtmp_play_session>(
        worker_,
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
        [self]() { self->shutdown_with_stage(runtime_end_reason::runtime_error, "media", "playback_ended"); });

    boost::asio::post(worker_.io(),
                      [self]()
                      {
                          if (self->rtmp_context_ == nullptr || !self->play_)
                          {
                              return;
                          }
                          if (rtmp_server_start(self->rtmp_context_, 0, nullptr) != 0)
                          {
                              self->shutdown_with_stage(runtime_end_reason::runtime_error, "control", "rtmp_play_start_failed");
                              return;
                          }
                          self->play_->startup();
                          spdlog::info("rtmp play {}", self->stream_name_);
                      });

    return RTMP_SERVER_ASYNC_START;
}

int rtmp_session::on_publish(std::string app, std::string stream)
{
    if (ending_ || closed_)
    {
        return -1;
    }
    if (publish_claim_pending_)
    {
        shutdown_with_stage(runtime_end_reason::protocol_error, "control", "duplicate_publish_request");
        return -1;
    }
    if (publish_ || play_)
    {
        return -1;
    }

    const auto target = parse_rtmp_publish_target(app, stream);
    if (!target)
    {
        shutdown_with_stage(runtime_end_reason::protocol_error, "control", "invalid_publish_target");
        return -1;
    }

    stream_id_ = target->stream_id;
    stream_name_ = target->stream_name;
    publish_claim_pending_ = true;
    const auto self = shared_from_this();
    boost::asio::spawn(
        worker_.io(),
        [self](boost::asio::yield_context yield) { self->run_publish_claim(yield); },
        boost::asio::bind_cancellation_slot(publish_claim_cancellation_.slot(), boost::asio::detached));
    return RTMP_SERVER_ASYNC_START;
}

void rtmp_session::run_publish_claim(boost::asio::yield_context yield)
{
    yield.throw_if_cancelled(false);
    const auto result = signaling_client::instance().claim_publish(stream_id_, "rtmp", stream_name_, yield);
    if (ending_ || closed_ || rtmp_context_ == nullptr || !publish_claim_pending_)
    {
        return;
    }

    publish_claim_pending_ = false;
    if (result.kind != signaling_result_kind::accepted)
    {
        spdlog::warn("rtmp publish claim failed stream {} stream_id {} status {} error {}", stream_name_, stream_id_, result.status, result.error);
        static_cast<void>(rtmp_server_start(rtmp_context_, -1, "publish claim rejected"));
        shutdown_with_stage(runtime_end_reason::requested, "claim", "publish_claim_rejected");
        return;
    }

    emit_starting();
    const auto self = shared_from_this();
    auto publish = std::make_shared<rtmp_publish_session>(
        worker_,
        stream_name_,
        initial_tracks_timeout_,
        [self]() { self->shutdown_with_stage(runtime_end_reason::runtime_error, "media", "publish_session_failed"); },
        [self](runtime_end_reason reason, std::string stage, std::string error)
        { self->shutdown_with_stage(reason, std::move(stage), std::move(error)); },
        [self]() { self->emit_streaming(); });
    if (!publish->startup())
    {
        static_cast<void>(rtmp_server_start(rtmp_context_, -1, "publish startup failed"));
        shutdown_with_stage(runtime_end_reason::runtime_error, "setup", "publish_startup_failed");
        return;
    }

    publish_ = std::move(publish);
    if (rtmp_server_start(rtmp_context_, 0, nullptr) != 0)
    {
        shutdown_with_stage(runtime_end_reason::runtime_error, "control", "rtmp_publish_start_failed");
        return;
    }
    spdlog::info("rtmp publish {}", stream_name_);
}

void rtmp_session::shutdown(runtime_end_reason reason, std::string error) { shutdown_with_stage(reason, {}, std::move(error)); }

void rtmp_session::shutdown_with_stage(runtime_end_reason reason, std::string stage, std::string error)
{
    const auto self = shared_from_this();
    boost::asio::dispatch(worker_.io(),
                          [self, reason, stage = std::move(stage), error = std::move(error)]() mutable
                          {
                              if (self->ending_ || self->closed_)
                              {
                                  return;
                              }
                              self->ending_ = true;
                              self->end_reason_ = reason;
                              self->end_stage_ = std::move(stage);
                              self->end_error_ = std::move(error);
                              boost::asio::post(self->worker_.io(), [self]() { self->safe_shutdown(); });
                          });
}

void rtmp_session::safe_shutdown()
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    publish_claim_cancellation_.emit(boost::asio::cancellation_type::all);
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
    transport_.shutdown();
    emit_stopped();
}

void rtmp_session::emit_starting()
{
    if (runtime_started_ || ending_ || closed_)
    {
        return;
    }
    runtime_started_ = true;
    event_reporter::instance().report(runtime_event{
        .kind = runtime_kind::publisher,
        .stream_id = stream_id_,
        .stream_name = stream_name_,
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::starting,
        .stage = "publish",
    });
}

void rtmp_session::emit_streaming()
{
    if (!runtime_started_ || runtime_streaming_ || ending_ || closed_)
    {
        return;
    }
    runtime_streaming_ = true;
    event_reporter::instance().report(runtime_event{
        .kind = runtime_kind::publisher,
        .stream_id = stream_id_,
        .stream_name = stream_name_,
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::streaming,
        .stage = "streaming",
    });
}

void rtmp_session::emit_stopped()
{
    if (!runtime_started_)
    {
        return;
    }
    runtime_started_ = false;
    runtime_streaming_ = false;
    event_reporter::instance().report(runtime_event{
        .kind = runtime_kind::publisher,
        .stream_id = stream_id_,
        .stream_name = stream_name_,
        .protocol = runtime_protocol::rtmp,
        .state = runtime_state::stopped,
        .stage = end_stage_.empty() ? std::nullopt : std::optional<std::string>{end_stage_},
        .end_reason = end_reason_,
        .error = end_error_.empty() ? std::nullopt : std::optional<std::string>{end_error_},
    });
}

std::string rtmp_session::make_stream_name(std::string_view app, std::string_view stream)
{
    if (app.empty())
    {
        return std::string(stream);
    }
    if (stream.empty())
    {
        return std::string(app);
    }
    return std::string(app) + '/' + std::string(stream);
}

}    // namespace media_server
