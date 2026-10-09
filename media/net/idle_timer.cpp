#include <utility>

#include "media/net/idle_timer.h"

namespace media_server
{

idle_timer::idle_timer(boost::asio::io_context& io) : timer_(io) {}

void idle_timer::start(std::weak_ptr<void> owner, std::chrono::steady_clock::duration timeout, std::function<void()> on_idle)
{
    owner_ = std::move(owner);
    timeout_ = timeout;
    on_idle_ = std::move(on_idle);
    touch();
    wait();
}

void idle_timer::touch() noexcept { last_activity_ = std::chrono::steady_clock::now(); }

void idle_timer::stop()
{
    on_idle_ = {};
    owner_.reset();
    timer_.cancel();
}

void idle_timer::wait()
{
    timer_.expires_at(last_activity_ + timeout_);
    timer_.async_wait(
        [this, owner = owner_](const boost::system::error_code& error)
        {
            const auto locked = owner.lock();
            if (error || !locked || !on_idle_)
            {
                return;
            }
            if (std::chrono::steady_clock::now() < last_activity_ + timeout_)
            {
                wait();
                return;
            }
            auto handler = std::move(on_idle_);
            handler();
        });
}

}    // namespace media_server
