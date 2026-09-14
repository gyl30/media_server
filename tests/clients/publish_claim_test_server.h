#ifndef MEDIA_SERVER_TESTS_CLIENTS_PUBLISH_CLAIM_TEST_SERVER_H
#define MEDIA_SERVER_TESTS_CLIENTS_PUBLISH_CLAIM_TEST_SERVER_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

namespace media_server::test
{

struct publish_claim_request
{
    std::string target;
    std::string body;
};

class publish_claim_test_server final
{
   public:
    explicit publish_claim_test_server(boost::beast::http::status status = boost::beast::http::status::ok,
                                       bool hold_response = false)
        : acceptor_(io_, {boost::asio::ip::address_v4::loopback(), 0}),
          port_(acceptor_.local_endpoint().port()),
          status_(status),
          hold_response_(hold_response),
          thread_([this]() { run(); })
    {
    }

    ~publish_claim_test_server()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            response_released_ = true;
        }
        condition_.notify_all();
        boost::asio::ip::tcp::socket wake(io_);
        boost::system::error_code ignored;
        wake.connect({boost::asio::ip::address_v4::loopback(), port_}, ignored);
        thread_.join();
    }

    publish_claim_test_server(const publish_claim_test_server&) = delete;
    publish_claim_test_server& operator=(const publish_claim_test_server&) = delete;

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    [[nodiscard]] std::size_t request_count() const
    {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

    publish_claim_request wait_request()
    {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, std::chrono::seconds(2), [this]() { return !requests_.empty(); }))
        {
            throw std::runtime_error("publish claim request timeout");
        }
        return requests_.front();
    }

    [[nodiscard]] bool wait_target(std::string_view target)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this, target]()
            {
                return std::ranges::any_of(requests_, [target](const auto& request) { return request.target == target; });
            });
    }

    void release_response()
    {
        {
            std::lock_guard lock(mutex_);
            response_released_ = true;
        }
        condition_.notify_all();
    }

   private:
    void run()
    {
        for (;;)
        {
            boost::asio::ip::tcp::socket socket(io_);
            boost::system::error_code error;
            acceptor_.accept(socket, error);
            if (error)
            {
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (stopping_)
                {
                    return;
                }
            }

            boost::beast::flat_buffer buffer;
            boost::beast::http::request<boost::beast::http::string_body> request;
            boost::beast::http::read(socket, buffer, request, error);
            if (error)
            {
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back({std::string(request.target()), request.body()});
            }
            condition_.notify_all();

            if (hold_response_)
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return response_released_ || stopping_; });
                if (stopping_)
                {
                    return;
                }
            }

            boost::beast::http::response<boost::beast::http::string_body> response(status_, request.version());
            response.set(boost::beast::http::field::content_type, "application/json");
            response.body() = response.result_int() >= 200 && response.result_int() < 300 ? R"({"result":"ok"})" : R"({"error":"rejected"})";
            response.prepare_payload();
            boost::beast::http::write(socket, response, error);
        }
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_{};
    boost::beast::http::status status_;
    bool hold_response_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<publish_claim_request> requests_;
    bool response_released_{};
    bool stopping_{};
    std::jthread thread_;
};

}    // namespace media_server::test

#endif
