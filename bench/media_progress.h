#ifndef MEDIA_SERVER_BENCH_MEDIA_PROGRESS_H
#define MEDIA_SERVER_BENCH_MEDIA_PROGRESS_H

#include <chrono>
#include <cstdint>
#include <memory>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>

namespace media_server::bench
{

struct media_counters
{
    std::uint64_t video_bytes{};
    std::uint64_t video_messages{};
    std::uint64_t audio_bytes{};
    std::uint64_t audio_messages{};
};

template <typename Client>
media_counters sample_media(const Client& client)
{
    return {client.received_bytes(), client.received_messages(), client.received_audio_bytes(), client.received_audio_messages()};
}

struct media_progress
{
    std::uint64_t video{};
    std::uint64_t audio{};
    std::chrono::steady_clock::time_point sampled{};
    bool stalled{};
};

template <typename Client>
bool media_stalled(const Client& client, const media_progress& progress, bool expect_audio)
{
    return client.received_messages() == progress.video || (expect_audio && client.received_audio_messages() == progress.audio);
}

template <typename Client>
boost::asio::awaitable<void> watch_media(std::shared_ptr<Client> client,
                                         std::shared_ptr<boost::asio::steady_timer> timer,
                                         std::shared_ptr<media_progress> progress,
                                         bool expect_audio,
                                         std::chrono::seconds interval,
                                         std::chrono::steady_clock::time_point deadline)
{
    progress->video = client->received_messages();
    progress->audio = client->received_audio_messages();
    progress->sampled = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() + interval < deadline)
    {
        timer->expires_after(interval);
        boost::system::error_code error;
        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error)
        {
            co_return;
        }
        if (media_stalled(*client, *progress, expect_audio))
        {
            progress->stalled = true;
            client->cancel();
            co_return;
        }
        progress->video = client->received_messages();
        progress->audio = client->received_audio_messages();
        progress->sampled = std::chrono::steady_clock::now();
    }
}

}    // namespace media_server::bench

#endif
