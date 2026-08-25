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

namespace
{
struct state
{
    std::mutex mutex;
    std::map<std::string, session_entry, std::less<>> sessions;
    std::map<output_key, output_entry> outputs;
};

std::shared_ptr<state> runtime()
{
    static auto value = std::make_shared<state>();
    return value;
}
}    // namespace

gb28181_create_error create(boost::asio::io_context& owner,
                            std::string stream_name,
                            gb28181_description description,
                            std::optional<boost::asio::ip::udp::endpoint> remote_rtp_endpoint,
                            std::optional<std::uint16_t> remote_rtcp_port)
{
    if (stream_name.empty() || description.rtp_port == 0 || description.payload_type > 127 ||
        (description.transport != gb28181_transport::udp && description.transport != gb28181_transport::tcp_active &&
         description.transport != gb28181_transport::tcp_passive))
    {
        return gb28181_create_error::invalid_configuration;
    }
    if (description.transport == gb28181_transport::udp)
    {
        if (description.rtcp_port == 0 || description.rtcp_port == description.rtp_port || !remote_rtcp_port || *remote_rtcp_port == 0)
        {
            return gb28181_create_error::invalid_configuration;
        }
        if (remote_rtp_endpoint && (remote_rtp_endpoint->port() == 0 || remote_rtp_endpoint->address().is_unspecified() ||
                                    remote_rtp_endpoint->address().is_v4() != description.address.is_v4()))
        {
            return gb28181_create_error::invalid_configuration;
        }
    }
    else if (description.rtcp_port != 0 || remote_rtp_endpoint || remote_rtcp_port)
    {
        return gb28181_create_error::invalid_configuration;
    }
    if (description.transport == gb28181_transport::tcp_active && description.address.is_unspecified())
    {
        return gb28181_create_error::invalid_configuration;
    }
    if (registry::instance().find(stream_name))
    {
        return gb28181_create_error::stream_conflict;
    }

    auto current = runtime();
    auto generation = std::make_shared<generation_token>();
    {
        std::scoped_lock lock(current->mutex);
        std::erase_if(current->sessions, [](const auto& entry) { return resource_expired(entry.second.resource); });
        if (!current->sessions.emplace(stream_name, session_entry{.generation = generation, .resource = std::monostate{}}).second)
        {
            return gb28181_create_error::duplicate_stream;
        }
    }

    const std::weak_ptr<state> weak_state = current;
    auto start_tcp_session =
        [weak_state, stream_name, description, generation](boost::asio::ip::tcp::socket socket) mutable
    {
        const auto shared_state = weak_state.lock();
        if (!shared_state)
        {
            return;
        }

        {
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->sessions.find(stream_name);
            if (iterator == shared_state->sessions.end() || iterator->second.generation != generation)
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
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->sessions.find(stream_name);
            if (iterator != shared_state->sessions.end() && iterator->second.generation == generation)
            {
                shared_state->sessions.erase(iterator);
            }
            return;
        }

        bool installed = false;
        {
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->sessions.find(stream_name);
            if (iterator != shared_state->sessions.end() && iterator->second.generation == generation)
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
            std::scoped_lock lock(current->mutex);
            const auto iterator = current->sessions.find(stream_name);
            if (iterator != current->sessions.end() && iterator->second.generation == generation)
            {
                current->sessions.erase(iterator);
            }
            return gb28181_create_error::internal_error;
        }
        resource = std::weak_ptr<gb28181_udp_session>{session};
    }
    else if (description.transport == gb28181_transport::tcp_active)
    {
        auto connector = std::make_shared<tcp_connector>(owner.get_executor());
        connector->startup(boost::asio::ip::tcp::endpoint{description.address, description.rtp_port},
                           tcp_establishment_timeout,
                           [weak_state, stream_name, generation, start_tcp_session = std::move(start_tcp_session)](
                               boost::system::error_code error, boost::asio::ip::tcp::socket socket) mutable
                           {
                               if (!error)
                               {
                                   start_tcp_session(std::move(socket));
                                   return;
                               }

                               if (const auto shared_state = weak_state.lock())
                               {
                                   std::scoped_lock lock(shared_state->mutex);
                                   const auto iterator = shared_state->sessions.find(stream_name);
                                   if (iterator != shared_state->sessions.end() && iterator->second.generation == generation)
                                   {
                                       shared_state->sessions.erase(iterator);
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
            std::scoped_lock lock(current->mutex);
            const auto iterator = current->sessions.find(stream_name);
            if (iterator != current->sessions.end() && iterator->second.generation == generation)
            {
                current->sessions.erase(iterator);
            }
            return gb28181_create_error::internal_error;
        }
        resource = std::weak_ptr<tcp_acceptor>{listener};
    }

    bool installed = false;
    {
        std::scoped_lock lock(current->mutex);
        const auto iterator = current->sessions.find(stream_name);
        if (iterator != current->sessions.end() && iterator->second.generation == generation)
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
        return gb28181_create_error::internal_error;
    }

    spdlog::info("gb28181 session created {}", stream_name);
    return gb28181_create_error::none;
}

gb28181_output_create_error create_output(boost::asio::io_context& owner,
                                          std::string stream_name,
                                          std::string output_id,
                                          bool rtcp,
                                          gb28181_description description)
{
    if (stream_name.empty() || output_id.empty() || description.rtp_port == 0 || description.payload_type > 127 ||
        (description.transport != gb28181_transport::udp && description.transport != gb28181_transport::tcp_active &&
         description.transport != gb28181_transport::tcp_passive))
    {
        return gb28181_output_create_error::invalid_configuration;
    }
    if (description.transport == gb28181_transport::udp &&
        (description.address.is_unspecified() || description.rtcp_port == 0 || description.rtcp_port == description.rtp_port))
    {
        return gb28181_output_create_error::invalid_configuration;
    }
    if (description.transport != gb28181_transport::udp && description.rtcp_port != 0)
    {
        return gb28181_output_create_error::invalid_configuration;
    }
    if (rtcp && description.transport != gb28181_transport::udp)
    {
        return gb28181_output_create_error::invalid_configuration;
    }
    if (description.transport == gb28181_transport::tcp_active && description.address.is_unspecified())
    {
        return gb28181_output_create_error::invalid_configuration;
    }
    auto stream = registry::instance().find(stream_name);
    if (!stream)
    {
        return gb28181_output_create_error::stream_not_found;
    }
    if (!gb28181_output_media::supported_tracks(stream->tracks()))
    {
        return gb28181_output_create_error::unsupported_stream;
    }

    const output_key key{stream_name, output_id};
    auto current = runtime();
    auto generation = std::make_shared<generation_token>();
    {
        std::scoped_lock lock(current->mutex);
        std::erase_if(current->outputs, [](const auto& entry) { return resource_expired(entry.second.resource); });
        if (!current->outputs.emplace(key, output_entry{.generation = generation, .resource = std::monostate{}}).second)
        {
            return gb28181_output_create_error::duplicate_output;
        }
    }

    const std::weak_ptr<state> weak_state = current;
    const std::weak_ptr<media_stream> weak_stream = stream;
    auto start_tcp_output =
        [weak_state, weak_stream, key, stream_name, description, generation](boost::asio::ip::tcp::socket socket) mutable
    {
        const auto shared_state = weak_state.lock();
        if (!shared_state)
        {
            return;
        }

        {
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->outputs.find(key);
            if (iterator == shared_state->outputs.end() || iterator->second.generation != generation)
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
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->outputs.find(key);
            if (iterator != shared_state->outputs.end() && iterator->second.generation == generation)
            {
                shared_state->outputs.erase(iterator);
            }
            return;
        }

        auto session = std::make_shared<gb28181_tcp_output_session>(std::move(socket), source, description.payload_type, description.ssrc);
        if (!session->startup())
        {
            session->shutdown();
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->outputs.find(key);
            if (iterator != shared_state->outputs.end() && iterator->second.generation == generation)
            {
                shared_state->outputs.erase(iterator);
            }
            return;
        }

        bool installed = false;
        {
            std::scoped_lock lock(shared_state->mutex);
            const auto iterator = shared_state->outputs.find(key);
            if (iterator != shared_state->outputs.end() && iterator->second.generation == generation)
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
            std::scoped_lock lock(current->mutex);
            const auto iterator = current->outputs.find(key);
            if (iterator != current->outputs.end() && iterator->second.generation == generation)
            {
                current->outputs.erase(iterator);
            }
            return gb28181_output_create_error::internal_error;
        }
        resource = std::weak_ptr<gb28181_udp_output_session>{session};
    }
    else if (description.transport == gb28181_transport::tcp_active)
    {
        auto connector = std::make_shared<tcp_connector>(owner.get_executor());
        connector->startup(boost::asio::ip::tcp::endpoint{description.address, description.rtp_port},
                           tcp_establishment_timeout,
                           [weak_state, key, generation, start_tcp_output = std::move(start_tcp_output)](boost::system::error_code error,
                                                                                                         boost::asio::ip::tcp::socket socket) mutable
                           {
                               if (!error)
                               {
                                   start_tcp_output(std::move(socket));
                                   return;
                               }

                               if (const auto shared_state = weak_state.lock())
                               {
                                   std::scoped_lock lock(shared_state->mutex);
                                   const auto iterator = shared_state->outputs.find(key);
                                   if (iterator != shared_state->outputs.end() && iterator->second.generation == generation)
                                   {
                                       shared_state->outputs.erase(iterator);
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
            std::scoped_lock lock(current->mutex);
            const auto iterator = current->outputs.find(key);
            if (iterator != current->outputs.end() && iterator->second.generation == generation)
            {
                current->outputs.erase(iterator);
            }
            return gb28181_output_create_error::internal_error;
        }
        resource = std::weak_ptr<tcp_acceptor>{listener};
    }

    bool installed = false;
    {
        std::scoped_lock lock(current->mutex);
        const auto iterator = current->outputs.find(key);
        if (iterator != current->outputs.end() && iterator->second.generation == generation)
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
        return gb28181_output_create_error::internal_error;
    }

    spdlog::info("gb28181 output created {} output {}", stream_name, output_id);
    return gb28181_output_create_error::none;
}

bool remove_output(std::string_view stream_name, std::string_view output_id)
{
    auto state = runtime();
    const output_key key{std::string{stream_name}, std::string{output_id}};
    output_resource resource;
    {
        std::scoped_lock lock(state->mutex);
        const auto iterator = state->outputs.find(key);
        if (iterator == state->outputs.end())
        {
            return false;
        }
        if (resource_expired(iterator->second.resource))
        {
            state->outputs.erase(iterator);
            return false;
        }
        resource = iterator->second.resource;
        state->outputs.erase(iterator);
    }
    shutdown_resource(resource);
    spdlog::info("gb28181 output removed {} output {}", stream_name, output_id);
    return true;
}

bool remove(std::string_view stream_name)
{
    auto state = runtime();
    session_resource resource;
    {
        std::scoped_lock lock(state->mutex);
        const auto iterator = state->sessions.find(stream_name);
        if (iterator == state->sessions.end())
        {
            return false;
        }
        if (resource_expired(iterator->second.resource))
        {
            state->sessions.erase(iterator);
            return false;
        }
        resource = iterator->second.resource;
        state->sessions.erase(iterator);
    }
    shutdown_resource(resource);
    spdlog::info("gb28181 session removed {}", stream_name);
    return true;
}

void shutdown()
{
    auto state = runtime();
    std::map<std::string, session_entry, std::less<>> sessions;
    std::map<output_key, output_entry> outputs;
    {
        std::scoped_lock lock(state->mutex);
        sessions = std::move(state->sessions);
        outputs = std::move(state->outputs);
        state->sessions.clear();
        state->outputs.clear();
    }
    for (const auto& [name, entry] : sessions)
    {
        static_cast<void>(name);
        shutdown_resource(entry.resource);
    }
    for (const auto& [name, entry] : outputs)
    {
        static_cast<void>(name);
        shutdown_resource(entry.resource);
    }
}

}    // namespace media_server::gb28181
