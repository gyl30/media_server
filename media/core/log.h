#ifndef MEDIA_CORE_LOG_H
#define MEDIA_CORE_LOG_H

#include <iostream>
#include <string_view>
#include <utility>

namespace media_server
{

template <typename... arguments>
void log_line(std::string_view module, arguments&&... values)
{
    std::clog << '[' << module << ']';
    ((std::clog << ' ' << std::forward<arguments>(values)), ...);
    std::clog << '\n';
}

}    // namespace media_server

#endif
