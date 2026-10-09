#ifndef MEDIA_SERVER_SERVICE_H
#define MEDIA_SERVER_SERVICE_H

#include <atomic>
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
    bool run_server();
    void fail();
    void stop();

   private:
    config config_;
    std::unique_ptr<worker_pool> workers_;
    // 启动失败或监听致命错误时由任意 worker 置 1，worker 线程全部结束后由 run() 读取。
    std::atomic_int result_{};
};

}    // namespace media_server

#endif
