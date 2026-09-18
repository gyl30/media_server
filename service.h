#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include <boost/asio/spawn.hpp>

#include "config.h"

namespace media_server
{

class io_context_pool;

class service
{
   public:
    explicit service(config cfg);
    ~service();

   public:
    int run();

   private:
    void register_signaling(boost::asio::yield_context& yield);
    void run_server(boost::asio::yield_context yield);
    void stop();

   private:
    config config_;
    std::unique_ptr<io_context_pool> workers_;
};

}    // namespace media_server

#endif
