#include <format>
#include <string>

#include <opus/opus.h>
#include <srtp2/srtp.h>
#include <boost/version.hpp>
#include <openssl/crypto.h>

#include "build_info.h"
#include "source_version.h"

#ifndef MEDIA_SERVER_SANITIZER_BUILD
#include <jemalloc/jemalloc.h>
#endif

extern "C"
{
#include <libavutil/avutil.h>
}

namespace media_server
{

std::string build_info()
{
#ifdef MEDIA_SERVER_SANITIZER_BUILD
    constexpr const char* allocator = "sanitizer";
#else
    constexpr const char* allocator = "jemalloc " JEMALLOC_VERSION;
#endif
    return std::format("media_server {} compiler {} boost {} {} ffmpeg {} {} {} allocator {}",
                       MEDIA_SERVER_SOURCE_VERSION,
                       __VERSION__,
                       BOOST_LIB_VERSION,
                       OpenSSL_version(OPENSSL_VERSION),
                       av_version_info(),
                       opus_get_version_string(),
                       srtp_get_version_string(),
                       allocator);
}

}    // namespace media_server
