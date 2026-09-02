#include "media/net/worker_context.h"

namespace media_server
{

worker_context::worker_context() : work_(boost::asio::make_work_guard(io_)) {}

boost::asio::io_context& worker_context::io() noexcept { return io_; }

void worker_context::stop() { io_.stop(); }

void worker_context::release_work() { work_.reset(); }

void worker_context::run() { io_.run(); }

}    // namespace media_server
