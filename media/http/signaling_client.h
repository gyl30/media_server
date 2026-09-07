#ifndef MEDIA_HTTP_SIGNALING_CLIENT_H
#define MEDIA_HTTP_SIGNALING_CLIENT_H

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>

namespace media_server
{

enum class signaling_result_kind
{
    accepted,
    rejected,
    temporary_failure,
    network_error,
};

struct signaling_request_result
{
    signaling_result_kind kind{signaling_result_kind::network_error};
    unsigned int status{};
    std::string error;
};

struct signaling_client_options
{
    std::string signaling_url;
    std::string server_id;
    std::string instance_id;
    std::string control_url;
    std::string media_ip;
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{5}};
    std::chrono::milliseconds request_timeout{std::chrono::seconds{3}};
};

class signaling_client
{
   public:
    signaling_client(boost::asio::io_context& io, signaling_client_options options);

    signaling_client(const signaling_client&) = delete;
    signaling_client& operator=(const signaling_client&) = delete;

    signaling_request_result register_once(boost::asio::yield_context& yield) const;
    signaling_request_result heartbeat_once(boost::asio::yield_context& yield) const;
    void run_heartbeat(boost::asio::yield_context& yield, std::function<void()> fenced_handler) const;

   private:
    signaling_request_result request(std::string_view target, std::string body, boost::asio::yield_context& yield) const;

    boost::asio::io_context& io_;
    signaling_client_options options_;
    std::string host_;
    std::string port_;
};

}    // namespace media_server

#endif
