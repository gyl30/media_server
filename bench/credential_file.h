#ifndef MEDIA_SERVER_BENCH_CREDENTIAL_FILE_H
#define MEDIA_SERVER_BENCH_CREDENTIAL_FILE_H

#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace media_server::bench
{

inline std::vector<std::string> read_credentials(const std::string& path, std::size_t count)
{
    std::ifstream input(path);
    if (!input)
    {
        throw std::runtime_error("cannot read credentials file " + path);
    }
    std::vector<std::string> values;
    std::unordered_set<std::string> unique;
    std::string value;
    while (std::getline(input, value))
    {
        if (!value.empty() && value.back() == '\r')
        {
            value.pop_back();
        }
        if (value.empty() || value.find_first_of(" \t") != std::string::npos || !unique.insert(value).second)
        {
            throw std::runtime_error("credentials file requires nonempty distinct values without whitespace");
        }
        values.push_back(std::move(value));
    }
    if (input.bad() || values.size() != count)
    {
        throw std::runtime_error("credentials file must contain exactly one value per session");
    }
    return values;
}

}    // namespace media_server::bench

#endif
