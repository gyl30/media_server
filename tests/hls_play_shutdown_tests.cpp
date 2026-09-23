#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <boost/asio/post.hpp>

#include "media/hls/hls.h"
#include "media/hls/hls_play_session.h"
#include "media/hls/hls_segmenter.h"
#include "media/net/worker_context.h"

namespace media_server
{
namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void test_request_stop_cancels_hls_inactivity_timer()
{
    hls::shutdown();
    worker_context worker;
    std::promise<std::string> created_signal;
    auto created = created_signal.get_future();
    std::promise<void> returned_signal;
    auto returned = returned_signal.get_future();

    boost::asio::post(
        worker.io(),
        [&]()
        {
            const auto session = hls_play_session::create(
                worker, "00000000-0000-4000-8000-000000000001", "live/hls-worker-stop", std::make_shared<hls_segmenter>());
            created_signal.set_value(session->secret());
        });

    std::jthread runner([&]() {
        worker.run();
        returned_signal.set_value();
    });
    const auto secret = created.get();
    worker.request_stop();
    const auto returned_in_time = returned.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    if (!returned_in_time)
    {
        worker.stop();
    }
    runner.join();

    require(returned_in_time, "worker stop cancels hls inactivity timer");
    require(!hls_play_session::find(secret, "live/hls-worker-stop"), "worker stop removes hls viewer");
    hls::shutdown();
}

}    // namespace
}    // namespace media_server

int main()
{
    try
    {
        media_server::test_request_stop_cancels_hls_inactivity_timer();
    }
    catch (const std::exception& error)
    {
        return 1;
    }
    return 0;
}
