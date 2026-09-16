#ifndef MEDIA_CORE_STREAM_ID_H
#define MEDIA_CORE_STREAM_ID_H

#include <stdexcept>
#include <string_view>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/string_generator.hpp>

namespace media_server
{

[[nodiscard]] inline bool valid_stream_id(std::string_view value)
{
    try
    {
        const auto id = boost::uuids::string_generator{}(value.begin(), value.end());
        return id.version() == boost::uuids::uuid::version_random_number_based && id.variant() == boost::uuids::uuid::variant_rfc_4122 &&
               boost::uuids::to_string(id) == value;
    }
    catch (const std::runtime_error&)
    {
        return false;
    }
}

}    // namespace media_server

#endif
