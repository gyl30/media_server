cmake_minimum_required(VERSION 3.20)
project(media_server LANGUAGES C CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckLinkerFlag)
include(CMakePushCheckState)
include(cmake/boost.cmake)
include(cmake/ffmpeg.cmake)
include(cmake/ireader.cmake)
include(cmake/srtp.cmake)

option(MEDIA_SERVER_HARDENING "Enable production compiler and linker hardening" ON)
set(MEDIA_SERVER_SANITIZER "none" CACHE STRING "Sanitizer: none, address, undefined, thread")
set_property(CACHE MEDIA_SERVER_SANITIZER PROPERTY STRINGS none address undefined thread)

set(media_server_sanitizers none address undefined thread)
if(NOT MEDIA_SERVER_SANITIZER IN_LIST media_server_sanitizers)
    message(FATAL_ERROR "MEDIA_SERVER_SANITIZER must be none, address, undefined, or thread")
endif()
if(MEDIA_SERVER_SANITIZER STREQUAL "none")
    pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
endif()

add_library(media_server_build_options INTERFACE)
add_library(media_server_link_options INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(media_server_build_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
        -Wimplicit-fallthrough
        -Werror=format-security
        -Werror
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16 AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
        target_compile_options(media_server_build_options INTERFACE
            -Wno-array-bounds
            -Wno-stringop-overflow
            -Wno-mismatched-new-delete
        )
    endif()

    function(media_server_add_compile_option_if_supported option)
        string(MAKE_C_IDENTIFIER "MEDIA_SERVER_CXX_${option}" variable)
        check_cxx_compiler_flag("${option}" ${variable})
        if(${variable})
            target_compile_options(media_server_build_options INTERFACE "${option}")
        endif()
    endfunction()

    function(media_server_add_link_option_if_supported option)
        string(MAKE_C_IDENTIFIER "MEDIA_SERVER_LINK_${option}" variable)
        check_linker_flag(CXX "${option}" ${variable})
        if(${variable})
            target_link_options(media_server_link_options INTERFACE "${option}")
        endif()
    endfunction()

    function(media_server_add_optimized_compile_option_if_supported option)
        string(MAKE_C_IDENTIFIER "MEDIA_SERVER_CXX_OPTIMIZED_${option}" variable)
        check_cxx_compiler_flag("${option}" ${variable})
        if(${variable})
            target_compile_options(media_server_build_options INTERFACE
                "$<${media_server_optimized_config}:${option}>")
        endif()
    endfunction()

    set(media_server_optimized_config
        "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")

    if(MEDIA_SERVER_SANITIZER STREQUAL "address" OR
       MEDIA_SERVER_SANITIZER STREQUAL "undefined")
        if(MEDIA_SERVER_SANITIZER STREQUAL "address")
            set(media_server_sanitizer_flags address,undefined)
            target_compile_definitions(media_server_build_options INTERFACE
                BOOST_USE_UCONTEXT
                BOOST_USE_ASAN
            )

            cmake_push_check_state(RESET)
            set(CMAKE_REQUIRED_DEFINITIONS -DBOOST_USE_UCONTEXT -DBOOST_USE_ASAN)
            set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
            set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
            set(CMAKE_REQUIRED_LIBRARIES Boost::context)
            set(CMAKE_REQUIRED_INCLUDES "${Boost_INCLUDE_DIRS}")
            unset(MEDIA_SERVER_BOOST_CONTEXT_ASAN_COMPATIBLE CACHE)
            check_cxx_source_compiles([=[
                #include <utility>
                #include <boost/context/fiber.hpp>

                int main()
                {
                    boost::context::fiber context([](boost::context::fiber&& caller) {
                        return std::move(caller);
                    });
                    return 0;
                }
            ]=] MEDIA_SERVER_BOOST_CONTEXT_ASAN_COMPATIBLE)
            cmake_pop_check_state()
            if(NOT MEDIA_SERVER_BOOST_CONTEXT_ASAN_COMPATIBLE)
                message(FATAL_ERROR
                    "MEDIA_SERVER_SANITIZER=address requires BOOST_CONTEXT_LIBRARY "
                    "built with context-impl=ucontext and sanitizer options")
            endif()
        else()
            set(media_server_sanitizer_flags undefined)
        endif()
        target_compile_options(media_server_build_options INTERFACE
            -U_FORTIFY_SOURCE
            -O1
            -g
            -fsanitize=${media_server_sanitizer_flags}
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fno-common
        )
        target_compile_options(ireader_media PRIVATE
            -U_FORTIFY_SOURCE
            -O1
            -g
            -fsanitize=${media_server_sanitizer_flags}
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fno-common
        )
        target_link_options(media_server_link_options INTERFACE
            -fsanitize=${media_server_sanitizer_flags}
        )
    elseif(MEDIA_SERVER_SANITIZER STREQUAL "thread")
        target_compile_options(media_server_build_options INTERFACE
            -U_FORTIFY_SOURCE
            -O2
            -g
            -fsanitize=thread
        )
        target_compile_options(ireader_media PRIVATE
            -U_FORTIFY_SOURCE
            -O2
            -g
            -fsanitize=thread
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(media_server_build_options INTERFACE
                -Wno-tsan
            )
        endif()
        target_link_options(media_server_link_options INTERFACE
            -fsanitize=thread
        )
    elseif(MEDIA_SERVER_HARDENING)
        target_compile_definitions(media_server_build_options INTERFACE
            _GLIBCXX_ASSERTIONS
        )

        target_compile_options(media_server_build_options INTERFACE
            "$<${media_server_optimized_config}:-U_FORTIFY_SOURCE>"
            "$<${media_server_optimized_config}:-D_FORTIFY_SOURCE=3>"
        )

        media_server_add_compile_option_if_supported(-fstack-protector-strong)
        media_server_add_compile_option_if_supported(-fstack-clash-protection)
        media_server_add_optimized_compile_option_if_supported(-fno-delete-null-pointer-checks)
        media_server_add_optimized_compile_option_if_supported(-fno-strict-overflow)
        media_server_add_optimized_compile_option_if_supported(-fno-strict-aliasing)
        media_server_add_optimized_compile_option_if_supported(-ftrivial-auto-var-init=zero)
        media_server_add_compile_option_if_supported(-fPIE)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            media_server_add_compile_option_if_supported(-Wtrampolines)
            media_server_add_compile_option_if_supported(-Wbidi-chars=any)
            media_server_add_optimized_compile_option_if_supported(-fzero-init-padding-bits=all)
        endif()

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            media_server_add_compile_option_if_supported(-fcf-protection=full)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            media_server_add_compile_option_if_supported(-mbranch-protection=standard)
        endif()

        if(UNIX AND NOT APPLE)
            media_server_add_link_option_if_supported(-pie)
            media_server_add_link_option_if_supported(-Wl,-z,noexecstack)
            media_server_add_link_option_if_supported(-Wl,-z,relro)
            media_server_add_link_option_if_supported(-Wl,-z,now)
        endif()
    endif()
elseif(NOT MEDIA_SERVER_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "MEDIA_SERVER_SANITIZER requires GCC or Clang")
endif()

add_library(media_core STATIC
    media/core/log.cpp
    media/core/media_stream.cpp
    media/core/stream_registry.cpp
    media/control/control_session_registry.cpp
    media/ps/mpeg_ps_output.cpp
    media/codec/audio_transcoder.cpp
    media/codec/video_transcoder.cpp
    media/codec/codec_utils.cpp
    media/net/worker_context.cpp
    media/net/io_context_pool.cpp
    media/net/port_manager.cpp
    media/net/tcp_write_queue.cpp
    media/net/tcp_yield_transport.cpp
    media/net/udp_yield_transport.cpp
    media/net/tcp_listener.cpp
    media/flv/flv_muxer.cpp
    media/hls/hls_play_session.cpp
    media/hls/hls_segmenter.cpp
    media/hls/hls.cpp
    media/http/http_flv_session.cpp
    media/http/hls_http_session.cpp
    media/http/gb28181_json.cpp
    media/http/gb28181_http.cpp
    media/http/rtsp_pull_http.cpp
    media/http/whep_http.cpp
    media/http/whip_http.cpp
    media/http/http_session.cpp
    media/gb28181/gb28181_rtp_receiver.cpp
    media/gb28181/gb28181_rtp_sender.cpp
    media/gb28181/gb28181_udp_receiver_session.cpp
    media/gb28181/gb28181_udp_sender_session.cpp
    media/gb28181/gb28181_tcp_sender_session.cpp
    media/gb28181/gb28181_tcp_receiver_session.cpp
    media/webrtc/dtls_certificate.cpp
    media/webrtc/dtls_transport.cpp
    media/webrtc/stun_message.cpp
    media/webrtc/srtp_transport.cpp
    media/webrtc/webrtc_sdp.cpp
    media/webrtc/whip_session.cpp
    media/webrtc/whip.cpp
    media/webrtc/whip_media_receiver.cpp
    media/webrtc/whep_session.cpp
    media/webrtc/whep_audio_egress.cpp
    media/webrtc/whep.cpp
    media/webrtc/webrtc_packetizer.cpp
    media/rtmp/rtmp_publish_session.cpp
    media/rtmp/rtmp_play_session.cpp
    media/rtmp/rtmp_session.cpp
    media/rtsp/rtsp_sdp.cpp
    media/rtsp/rtsp_uri.cpp
    media/rtsp/rtsp_pull_session.cpp
    media/rtsp/rtsp_pull_media.cpp
    media/rtsp/rtsp_publish_session.cpp
    media/rtsp/rtsp_publish_tcp_session.cpp
    media/rtsp/rtsp_publish_udp_session.cpp
    media/rtsp/rtsp_play_session.cpp
    media/rtsp/rtsp_publish_media.cpp
    media/rtsp/rtsp_server_connection.cpp
)

target_include_directories(media_core PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_include_directories(media_core SYSTEM PUBLIC
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(media_core PUBLIC
    media_server_build_options
    ireader_media
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    FFmpeg::audio
    FFmpeg::scale
    libSRTP::srtp2
    spdlog::spdlog
    ${MEDIA_SERVER_BOOST_LIBS}
)

add_executable(media_server
    main.cpp
    config.cpp
    service.cpp
)
target_link_libraries(media_server PRIVATE
    media_core
    media_server_link_options
    ${MEDIA_SERVER_BOOST_PROGRAM_OPTIONS_LIB}
)
if(MEDIA_SERVER_SANITIZER STREQUAL "none")
    target_link_libraries(media_server PRIVATE PkgConfig::JEMALLOC)
endif()
