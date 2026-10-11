#ifndef MEDIA_SERVER_BENCH_MEDIA_PROGRESS_H
#define MEDIA_SERVER_BENCH_MEDIA_PROGRESS_H

#include <chrono>
#include <memory>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>

namespace media_server::bench
{

template <typename Client>
boost::asio::awaitable<void> watch_media(std::shared_ptr<Client> client,
                                       std::shared_ptr<boost::asio::steady_timer> timer,
                                       std::shared_ptr<bool> stalled,
                                       bool expect_audio,
                                       std::chrono::seconds interval,
                                       std::chrono::steady_clock::time_point deadline)
{
    auto video = client->received_messages();
    auto audio = client->received_audio_messages();
    while (std::chrono::steady_clock::now() + interval < deadline)
    {
        timer->expires_after(interval);
        boost::system::error_code error;
        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error)
        {
            co_return;
        }
        if (client->received_messages() == video || (expect_audio && client->received_audio_messages() == audio))
        {
            *stalled = true;
            client->cancel();
            co_return;
        }
        video = client->received_messages();
        audio = client->received_audio_messages();
    }
}

}    // namespace media_server::bench

#endif
