#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <utility>

#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>

#include "media/http/http_event.h"
#include "media/net/worker_context.h"
#include "media/hls/hls_play_session.h"

namespace media_server
{
namespace
{

struct hls_play_sessions
{
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<hls_play_session>, std::less<>> by_secret;
};

hls_play_sessions& sessions()
{
    static hls_play_sessions value;
    return value;
}

void remove_session(std::string_view secret, const hls_play_session* expected)
{
    auto& current = sessions();
    std::scoped_lock lock(current.mutex);
    const auto iterator = current.by_secret.find(secret);
    if (iterator != current.by_secret.end() && iterator->second.get() == expected)
    {
        current.by_secret.erase(iterator);
    }
}

}    // namespace

std::shared_ptr<hls_play_session> hls_play_session::create(
    worker_context& worker, std::string stream_id, std::string stream_name, std::shared_ptr<hls_segmenter> segmenter)
{
    boost::uuids::random_generator generator;
    auto& current = sessions();
    for (;;)
    {
        auto secret = boost::uuids::to_string(generator());
        auto session = std::shared_ptr<hls_play_session>(new hls_play_session(worker, stream_id, stream_name, std::move(secret), segmenter));
        {
            std::scoped_lock lock(current.mutex);
            if (!current.by_secret.emplace(session->secret(), session).second)
            {
                continue;
            }
        }
        session->wait_for_inactivity();
        return session;
    }
}

std::shared_ptr<hls_play_session> hls_play_session::find(std::string_view secret, std::string_view stream_name)
{
    std::shared_ptr<hls_play_session> session;
    {
        auto& current = sessions();
        std::scoped_lock lock(current.mutex);
        const auto iterator = current.by_secret.find(secret);
        if (iterator == current.by_secret.end())
        {
            return {};
        }
        session = iterator->second;
    }
    return session->matches(stream_name) ? session : std::shared_ptr<hls_play_session>{};
}

void hls_play_session::shutdown_all()
{
    auto& current = sessions();
    std::map<std::string, std::shared_ptr<hls_play_session>, std::less<>> detached;
    {
        std::scoped_lock lock(current.mutex);
        detached.swap(current.by_secret);
    }
}

hls_play_session::hls_play_session(
    worker_context& worker, std::string stream_id, std::string stream_name, std::string secret, std::shared_ptr<hls_segmenter> segmenter)
    : stream_id_(std::move(stream_id)),
      stream_name_(std::move(stream_name)),
      secret_(std::move(secret)),
      segmenter_(std::move(segmenter)),
      last_activity_(std::chrono::steady_clock::now()),
      timer_(worker.io())
{
}

bool hls_play_session::refresh()
{
    std::scoped_lock lock(mutex_);
    if (expired_)
    {
        return false;
    }
    last_activity_ = std::chrono::steady_clock::now();
    return true;
}

bool hls_play_session::mark_streaming()
{
    std::scoped_lock lock(mutex_);
    if (expired_ || streaming_)
    {
        return false;
    }
    streaming_ = true;
    return true;
}

void hls_play_session::wait_for_inactivity()
{
    std::chrono::steady_clock::time_point deadline;
    {
        std::scoped_lock lock(mutex_);
        deadline = last_activity_ + inactivity_timeout;
    }
    timer_.expires_at(deadline);
    const auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code& error) { self->handle_inactivity(error); });
}

void hls_play_session::handle_inactivity(const boost::system::error_code& error)
{
    if (error)
    {
        return;
    }

    bool expired = false;
    {
        std::scoped_lock lock(mutex_);
        if (expired_)
        {
            return;
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= last_activity_ + inactivity_timeout)
        {
            expired_ = true;
            expired = true;
        }
    }
    if (!expired)
    {
        wait_for_inactivity();
        return;
    }

    http_event::report_hls_output(event_state::timeout, stream_id_, stream_name_, "inactivity");
    http_event::report_hls_output(event_state::stopped, stream_id_, stream_name_);
    remove_session(secret_, this);
}

bool hls_play_session::matches(std::string_view stream_name) const
{
    std::scoped_lock lock(mutex_);
    return !expired_ && stream_name_ == stream_name;
}

}    // namespace media_server
