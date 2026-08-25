#include <utility>

#include "media/gb28181/gb28181_session_registry.h"

namespace media_server
{

gb28181_session_registry& gb28181_session_registry::instance()
{
    static gb28181_session_registry value;
    return value;
}

bool gb28181_session_registry::add_input(std::string stream_name, std::shared_ptr<gb28181_session> session)
{
    if (stream_name.empty() || !session)
    {
        return false;
    }
    std::scoped_lock lock(mutex_);
    return inputs_.emplace(std::move(stream_name), std::move(session)).second;
}

std::shared_ptr<gb28181_session> gb28181_session_registry::take_input(std::string_view stream_name)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = inputs_.find(stream_name);
    if (iterator == inputs_.end())
    {
        return {};
    }
    auto session = std::move(iterator->second);
    inputs_.erase(iterator);
    return session;
}

void gb28181_session_registry::remove_input(std::string_view stream_name, const gb28181_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = inputs_.find(stream_name);
    if (iterator != inputs_.end() && iterator->second.get() == &expected)
    {
        inputs_.erase(iterator);
    }
}

bool gb28181_session_registry::add_output(std::string stream_name,
                                          std::string output_id,
                                          std::shared_ptr<gb28181_session> session)
{
    if (stream_name.empty() || output_id.empty() || !session)
    {
        return false;
    }
    std::scoped_lock lock(mutex_);
    return outputs_.emplace(output_key{std::move(stream_name), std::move(output_id)}, std::move(session)).second;
}

std::shared_ptr<gb28181_session> gb28181_session_registry::take_output(std::string_view stream_name, std::string_view output_id)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = outputs_.find(output_key{std::string{stream_name}, std::string{output_id}});
    if (iterator == outputs_.end())
    {
        return {};
    }
    auto session = std::move(iterator->second);
    outputs_.erase(iterator);
    return session;
}

void gb28181_session_registry::remove_output(std::string_view stream_name,
                                              std::string_view output_id,
                                              const gb28181_session& expected)
{
    std::scoped_lock lock(mutex_);
    const auto iterator = outputs_.find(output_key{std::string{stream_name}, std::string{output_id}});
    if (iterator != outputs_.end() && iterator->second.get() == &expected)
    {
        outputs_.erase(iterator);
    }
}

void gb28181_session_registry::clear()
{
    std::map<std::string, std::shared_ptr<gb28181_session>, std::less<>> inputs;
    std::map<output_key, std::shared_ptr<gb28181_session>> outputs;
    {
        std::scoped_lock lock(mutex_);
        inputs.swap(inputs_);
        outputs.swap(outputs_);
    }
}

}    // namespace media_server
