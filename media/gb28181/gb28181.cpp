#include <map>
#include <mutex>
#include <chrono>
#include <utility>
#include <variant>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "media/net/tcp_acceptor.h"
#include "media/net/tcp_connector.h"
#include "media/gb28181/gb28181.h"
#include "media/gb28181/gb28181_tcp_session.h"
#include "media/gb28181/gb28181_udp_session.h"
#include "media/gb28181/gb28181_output_media.h"
#include "media/gb28181/gb28181_tcp_output_session.h"
#include "media/gb28181/gb28181_udp_output_session.h"

namespace media_server::gb28181
{
namespace
{

constexpr auto tcp_establishment_timeout = std::chrono::seconds(10);

struct generation_token
{
};

using session_resource = std::variant<std::monostate,
                                      std::weak_ptr<gb28181_udp_session>,
                                      std::weak_ptr<gb28181_tcp_session>,
                                      std::weak_ptr<tcp_connector>,
                                      std::weak_ptr<tcp_acceptor>>;

struct session_entry
{
    std::shared_ptr<generation_token> generation;
    session_resource resource;
};

using output_resource = std::variant<std::monostate,
                                     std::weak_ptr<gb28181_udp_output_session>,
                                     std::weak_ptr<gb28181_tcp_output_session>,
                                     std::weak_ptr<tcp_connector>,
                                     std::weak_ptr<tcp_acceptor>>;

struct output_entry
{
    std::shared_ptr<generation_token> generation;
    output_resource resource;
};

using output_key = std::pair<std::string, std::string>;

std::mutex mutex;
std::map<std::string, session_entry, std::less<>> sessions;
std::map<output_key, output_entry> outputs;

template <typename Resource>
bool resource_expired(const Resource& resource)
{
    return std::visit(
        [](const auto& value)
        {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>)
            {
                return false;
            }
            else
            {
                return value.expired();
            }
        },
        resource);
}

template <typename Resource>
void shutdown_resource(const Resource& resource)
{
    std::visit(
        [](const auto& value)
        {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<value_type, std::monostate>)
            {
                if (const auto object = value.lock())
                {
                    object->shutdown();
                }
            }
        },
        resource);
}

boost::asio::ip::address bind_address(const boost::asio::ip::address& address)
{
    return address.is_v4() ? boost::asio::ip::address{boost::asio::ip::address_v4::any()}
                           : boost::asio::ip::address{boost::asio::ip::address_v6::any()};
}

}    // namespace

int create(boost::asio::io_context& owner, gb28181_input_config config)
{
    auto stream_name = std::move(config.stream_name);
    auto description = std::move(config.description);
    auto remote_rtp_endpoint = std::move(config.remote_rtp_endpoint);
    auto remote_rtcp_port = config.remote_rtcp_port;

    if (registry::instance().find(stream_name))
    {
        spdlog::error("gb28181 stream conflict {}", stream_name);
        return -1;
    }

    auto generation = std::make_shared<generation_token>();
    {
        std::scoped_lock lock(mutex);
        std::erase_if(sessions, [](const auto& entry) { return resource_expired(entry.second.resource); });
        if (!sessions.emplace(stream_name, session_entry{.generation = generation, .resource = std::monostate{}}).second)
        {
            spdlog::error("gb28181 duplicate stream {}", stream_name);
            return -1;
        }
    }

    auto start_tcp_session =
        [stream_name, description, generation](boost::asio::ip::tcp::socket socket) mutable
    {
        {
            std::scoped_lock lock(mutex);
            const auto iterator = sessions.find(stream_name);
            if (iterator == sessions.end() || iterator->second.generation != generation)
            {
                boost::system::error_code error;
                socket.close(error);
                return;
            }
        }

        auto session = std::make_shared<gb28181_tcp_session>(std::move(socket), stream_name, description.payload_type, description.ssrc);
        if (!session->startup())
        {
            session->shutdown();
            std::scoped_lock lock(mutex);
            const auto iterator = sessions.find(stream_name);
            if (iterator != sessions.end() && iterator->second.generation == generation)
            {
                sessions.erase(iterator);
            }
            return;
        }

        bool installed = false;
        {
            std::scoped_lock lock(mutex);
            const auto iterator = sessions.find(stream_name);
            if (iterator != sessions.end() && iterator->second.generation == generation)
            {
                iterator->second.resource = std::weak_ptr<gb28181_tcp_session>{session};
                installed = true;
            }
        }
        if (!installed)
        {
            session->shutdown();
        }
    };

    session_resource resource;
    if (description.transport == gb28181_transport::udp)
    {
        auto session = std::make_shared<gb28181_udp_session>(
            owner.get_executor(), stream_name, description, gb28181_udp_peer{.rtp = std::move(remote_rtp_endpoint), .rtcp_port = *remote_rtcp_port});
        if (!session->startup())
        {
            std::scoped_lock lock(mutex);
            const auto iterator = sessions.find(stream_name);
            if (iterator != sessions.end() && iterator->second.generation == generation)
            {
                sessions.erase(iterator);
            }
            spdlog::error("gb28181 udp session startup failed stream {}", stream_name);
            return -1;
        }
        resource = std::weak_ptr<gb28181_udp_session>{session};
    }
    else if (description.transport == gb28181_transport::tcp_active)
    {
        auto connector = std::make_shared<tcp_connector>(owner.get_executor());
        connector->startup(boost::asio::ip::tcp::endpoint{description.address, description.rtp_port},
                           tcp_establishment_timeout,
                           [stream_name, generation, start_tcp_session = std::move(start_tcp_session)](
                               boost::system::error_code error, boost::asio::ip::tcp::socket socket) mutable
                           {
                               if (!error)
                               {
                                   start_tcp_session(std::move(socket));
                                   return;
                               }

                               {
                                   std::scoped_lock lock(mutex);
                                   const auto iterator = sessions.find(stream_name);
                                   if (iterator != sessions.end() && iterator->second.generation == generation)
                                   {
                                       sessions.erase(iterator);
                                   }
                               }
                               spdlog::warn("gb28181 tcp connect failed stream {} error {}", stream_name, error.message());
                           });
        resource = std::weak_ptr<tcp_connector>{connector};
    }
    else
    {
        auto listener = std::make_shared<tcp_acceptor>(owner.get_executor(), description.rtp_port, bind_address(description.address));
        const auto error = listener->startup(std::move(start_tcp_session), tcp_establishment_timeout);
        if (error)
        {
            std::scoped_lock lock(mutex);
            const auto iterator = sessions.find(stream_name);
            if (iterator != sessions.end() && iterator->second.generation == generation)
            {
                sessions.erase(iterator);
            }
            spdlog::error("gb28181 tcp listener startup failed stream {}", stream_name);
            return -1;
        }
        resource = std::weak_ptr<tcp_acceptor>{listener};
    }

    bool installed = false;
    {
        std::scoped_lock lock(mutex);
        const auto iterator = sessions.find(stream_name);
        if (iterator != sessions.end() && iterator->second.generation == generation)
        {
            if (std::holds_alternative<std::monostate>(iterator->second.resource))
            {
                iterator->second.resource = resource;
            }
            installed = true;
        }
    }
    if (!installed)
    {
        shutdown_resource(resource);
        spdlog::error("gb28181 session resource install failed stream {}", stream_name);
        return -1;
    }

    spdlog::info("gb28181 session created {}", stream_name);
    return 0;
}

int create_output(boost::asio::io_context& owner, gb28181_output_config config)
{
    auto stream_name = std::move(config.stream_name);
    auto output_id = std::move(config.output_id);
    auto description = std::move(config.description);
    const auto rtcp = config.rtcp;

    auto stream = registry::instance().find(stream_name);
    if (!stream)
    {
        spdlog::error("gb28181 output source stream not found {} output {}", stream_name, output_id);
        return -1;
    }
    if (!gb28181_output_media::supported_tracks(stream->tracks()))
    {
        spdlog::error("gb28181 output source stream unsupported {} output {}", stream_name, output_id);
        return -1;
    }

    const output_key key{stream_name, output_id};
    auto generation = std::make_shared<generation_token>();
    {
        std::scoped_lock lock(mutex);
        std::erase_if(outputs, [](const auto& entry) { return resource_expired(entry.second.resource); });
        if (!outputs.emplace(key, output_entry{.generation = generation, .resource = std::monostate{}}).second)
        {
            spdlog::error("gb28181 duplicate output stream {} output {}", stream_name, output_id);
            return -1;
        }
    }

    const std::weak_ptr<media_stream> weak_stream = stream;
    auto start_tcp_output =
        [weak_stream, key, stream_name, description, generation](boost::asio::ip::tcp::socket socket) mutable
    {
        {
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator == outputs.end() || iterator->second.generation != generation)
            {
                boost::system::error_code error;
                socket.close(error);
                return;
            }
        }

        const auto source = weak_stream.lock();
        if (!source)
        {
            boost::system::error_code error;
            socket.close(error);
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator != outputs.end() && iterator->second.generation == generation)
            {
                outputs.erase(iterator);
            }
            return;
        }

        auto session = std::make_shared<gb28181_tcp_output_session>(std::move(socket), source, description.payload_type, description.ssrc);
        if (!session->startup())
        {
            session->shutdown();
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator != outputs.end() && iterator->second.generation == generation)
            {
                outputs.erase(iterator);
            }
            return;
        }

        bool installed = false;
        {
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator != outputs.end() && iterator->second.generation == generation)
            {
                iterator->second.resource = std::weak_ptr<gb28181_tcp_output_session>{session};
                installed = true;
            }
        }
        if (!installed)
        {
            session->shutdown();
        }
    };

    output_resource resource;
    if (description.transport == gb28181_transport::udp)
    {
        auto session = std::make_shared<gb28181_udp_output_session>(owner.get_executor(), std::move(stream), description, rtcp);
        if (!session->startup())
        {
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator != outputs.end() && iterator->second.generation == generation)
            {
                outputs.erase(iterator);
            }
            spdlog::error("gb28181 udp output startup failed stream {} output {}", stream_name, output_id);
            return -1;
        }
        resource = std::weak_ptr<gb28181_udp_output_session>{session};
    }
    else if (description.transport == gb28181_transport::tcp_active)
    {
        auto connector = std::make_shared<tcp_connector>(owner.get_executor());
        connector->startup(boost::asio::ip::tcp::endpoint{description.address, description.rtp_port},
                           tcp_establishment_timeout,
                           [key, generation, start_tcp_output = std::move(start_tcp_output)](boost::system::error_code error,
                                                                                              boost::asio::ip::tcp::socket socket) mutable
                           {
                               if (!error)
                               {
                                   start_tcp_output(std::move(socket));
                                   return;
                               }

                               {
                                   std::scoped_lock lock(mutex);
                                   const auto iterator = outputs.find(key);
                                   if (iterator != outputs.end() && iterator->second.generation == generation)
                                   {
                                       outputs.erase(iterator);
                                   }
                               }
                               spdlog::warn("gb28181 tcp output connect failed stream {} output {} error {}", key.first, key.second, error.message());
                           });
        resource = std::weak_ptr<tcp_connector>{connector};
    }
    else
    {
        auto listener = std::make_shared<tcp_acceptor>(owner.get_executor(), description.rtp_port, bind_address(description.address));
        const auto error = listener->startup(std::move(start_tcp_output), tcp_establishment_timeout);
        if (error)
        {
            std::scoped_lock lock(mutex);
            const auto iterator = outputs.find(key);
            if (iterator != outputs.end() && iterator->second.generation == generation)
            {
                outputs.erase(iterator);
            }
            spdlog::error("gb28181 tcp output listener startup failed stream {} output {}", stream_name, output_id);
            return -1;
        }
        resource = std::weak_ptr<tcp_acceptor>{listener};
    }

    bool installed = false;
    {
        std::scoped_lock lock(mutex);
        const auto iterator = outputs.find(key);
        if (iterator != outputs.end() && iterator->second.generation == generation)
        {
            if (std::holds_alternative<std::monostate>(iterator->second.resource))
            {
                iterator->second.resource = resource;
            }
            installed = true;
        }
    }
    if (!installed)
    {
        shutdown_resource(resource);
        spdlog::error("gb28181 output resource install failed stream {} output {}", stream_name, output_id);
        return -1;
    }

    spdlog::info("gb28181 output created {} output {}", stream_name, output_id);
    return 0;
}

int remove_output(std::string_view stream_name, std::string_view output_id)
{
    const output_key key{std::string{stream_name}, std::string{output_id}};
    output_resource resource;
    {
        std::scoped_lock lock(mutex);
        const auto iterator = outputs.find(key);
        if (iterator == outputs.end())
        {
            spdlog::error("gb28181 output not found stream {} output {}", stream_name, output_id);
            return -1;
        }
        if (resource_expired(iterator->second.resource))
        {
            outputs.erase(iterator);
            spdlog::error("gb28181 output resource expired stream {} output {}", stream_name, output_id);
            return -1;
        }
        resource = iterator->second.resource;
        outputs.erase(iterator);
    }
    shutdown_resource(resource);
    spdlog::info("gb28181 output removed {} output {}", stream_name, output_id);
    return 0;
}

int remove(std::string_view stream_name)
{
    session_resource resource;
    {
        std::scoped_lock lock(mutex);
        const auto iterator = sessions.find(stream_name);
        if (iterator == sessions.end())
        {
            spdlog::error("gb28181 stream not found {}", stream_name);
            return -1;
        }
        if (resource_expired(iterator->second.resource))
        {
            sessions.erase(iterator);
            spdlog::error("gb28181 stream resource expired {}", stream_name);
            return -1;
        }
        resource = iterator->second.resource;
        sessions.erase(iterator);
    }
    shutdown_resource(resource);
    spdlog::info("gb28181 session removed {}", stream_name);
    return 0;
}

}    // namespace media_server::gb28181
