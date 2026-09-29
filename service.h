#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <memory>

#include "config.h"

namespace media_server
{

class worker_pool;

class service
{
   public:
    explicit service(config cfg);
    ~service();

   public:
    int run();

   private:
    void run_server();
    void stop();

   private:
    config config_;
    std::unique_ptr<worker_pool> workers_;
};

}    // namespace media_server

#endif
