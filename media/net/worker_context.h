#ifndef MEDIA_NET_WORKER_CONTEXT_H
#define MEDIA_NET_WORKER_CONTEXT_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

namespace media_server
{

class worker_context final
{
   public:
    worker_context();

    [[nodiscard]] boost::asio::io_context& io() noexcept;

    void stop();
    void release_work();
    void run();

   private:
    using work_guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    boost::asio::io_context io_{1};
    work_guard work_;
};

}    // namespace media_server

#endif
