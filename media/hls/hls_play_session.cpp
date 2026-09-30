#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <utility>

#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>

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

}    // namespace

std::shared_ptr<hls_play_session> hls_play_session::create(worker_context& worker,
                                                           std::string stream_name,
                                                           std::shared_ptr<hls_segmenter> segmenter)
{
    boost::uuids::random_generator generator;
    auto& current = sessions();
    for (;;)
    {
        auto secret = boost::uuids::to_string(generator());
        auto session = std::shared_ptr<hls_play_session>(new hls_play_session(worker, stream_name, std::move(secret), segmenter));
        {
            std::scoped_lock lock(current.mutex);
            if (!current.by_secret.emplace(session->secret(), session).second)
            {
                continue;
            }
        }
        session->shutdown_subscription_ = worker.subscribe_shutdown([session]() { session->safe_shutdown(); });
        if (!session->shutdown_subscription_)
        {
            session->safe_shutdown();
        }
        else
        {
            session->wait_for_inactivity();
        }
        return session;
    }
}

std::shared_ptr<hls_play_session> hls_play_session::find(std::string_view secret, std::string_view stream_name)
{
    auto& current = sessions();
    std::scoped_lock lock(current.mutex);
    const auto iterator = current.by_secret.find(secret);
    if (iterator == current.by_secret.end() || iterator->second->stream_name_ != stream_name)
    {
        return {};
    }
    return iterator->second;
}

hls_play_session::hls_play_session(
    worker_context& worker, std::string stream_name, std::string secret, std::shared_ptr<hls_segmenter> segmenter)
    : stream_name_(std::move(stream_name)),
      secret_(std::move(secret)),
      segmenter_(std::move(segmenter)),
      last_activity_(std::chrono::steady_clock::now()),
      timer_(worker.io())
{
}

void hls_play_session::refresh()
{
    auto& current = sessions();
    std::scoped_lock lock(current.mutex);
    const auto iterator = current.by_secret.find(secret_);
    if (iterator != current.by_secret.end() && iterator->second.get() == this)
    {
        last_activity_ = std::chrono::steady_clock::now();
    }
}

void hls_play_session::wait_for_inactivity()
{
    std::chrono::steady_clock::time_point deadline;
    {
        auto& current = sessions();
        std::scoped_lock lock(current.mutex);
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
        auto& current = sessions();
        std::scoped_lock lock(current.mutex);
        if (std::chrono::steady_clock::now() >= last_activity_ + inactivity_timeout)
        {
            current.by_secret.erase(secret_);
            expired = true;
        }
    }
    if (!expired)
    {
        wait_for_inactivity();
        return;
    }

    shutdown_subscription_.reset();
}

void hls_play_session::safe_shutdown()
{
    {
        auto& current = sessions();
        std::scoped_lock lock(current.mutex);
        current.by_secret.erase(secret_);
    }
    shutdown_subscription_.reset();
    timer_.cancel();
}

}    // namespace media_server
