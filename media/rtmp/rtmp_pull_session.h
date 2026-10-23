#ifndef MEDIA_RTMP_RTMP_PULL_SESSION_H
#define MEDIA_RTMP_RTMP_PULL_SESSION_H

#include <memory>
#include <string>
#include <cstddef>
#include <cstdint>

#include <boost/asio.hpp>

#include "media/net/idle_timer.h"
#include "media/net/tcp_transport.h"
#include "media/core/session_registry.h"

struct rtmp_client_t;

namespace media_server
{

class worker_context;
class rtmp_publish_session;

struct rtmp_pull_config
{
    std::string stream_id;
    std::string host;
    std::uint16_t port{1935};
    std::string app;
    std::string stream;
    std::string tc_url;
};

class rtmp_pull_session final : public session, public std::enable_shared_from_this<rtmp_pull_session>
{
   public:
    rtmp_pull_session(worker_context& worker, rtmp_pull_config config);
    ~rtmp_pull_session();

   public:
    void startup();
    void shutdown() override;

   private:
    static int send_callback(void* param, const void* header, std::size_t header_bytes, const void* payload, std::size_t payload_bytes);
    static int video_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int audio_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);
    static int script_callback(void* param, const void* data, std::size_t bytes, std::uint32_t timestamp);

   private:
    void run(boost::asio::yield_context yield);
    void run_read(rtmp_client_t* client, boost::asio::yield_context yield);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    rtmp_pull_config config_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket connect_socket_;
    idle_timer idle_timer_;
    std::shared_ptr<tcp_transport> transport_;
    std::unique_ptr<rtmp_publish_session> media_;
};

}    // namespace media_server

#endif
