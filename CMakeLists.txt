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

include(CTest)
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(cmake/boost.cmake)
include(cmake/ffmpeg.cmake)
include(cmake/ireader.cmake)
include(cmake/srtp.cmake)

option(MEDIA_SERVER_HARDENING "Enable production compiler and linker hardening" ON)
set(MEDIA_SERVER_SANITIZER "none" CACHE STRING "Sanitizer: none, address, thread")
set_property(CACHE MEDIA_SERVER_SANITIZER PROPERTY STRINGS none address thread)

set(media_server_sanitizers none address thread)
if(NOT MEDIA_SERVER_SANITIZER IN_LIST media_server_sanitizers)
    message(FATAL_ERROR "MEDIA_SERVER_SANITIZER must be none, address, or thread")
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

    if(MEDIA_SERVER_SANITIZER STREQUAL "address")
        target_compile_options(media_server_build_options INTERFACE
            -U_FORTIFY_SOURCE
            -O1
            -g
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fno-common
        )
        target_link_options(media_server_link_options INTERFACE
            -fsanitize=address,undefined
        )
    elseif(MEDIA_SERVER_SANITIZER STREQUAL "thread")
        target_compile_options(media_server_build_options INTERFACE
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
    media/codec/audio_transcoder.cpp
    media/codec/video_transcoder.cpp
    media/codec/codec_utils.cpp
    media/net/io_context_pool.cpp
    media/net/tcp_connection.cpp
    media/net/tcp_connector.cpp
    media/net/tcp_listener.cpp
    media/net/tcp_acceptor.cpp
    media/net/udp_socket.cpp
    media/flv/flv_output_muxer.cpp
    media/hls/hls_output.cpp
    media/hls/hls.cpp
    media/http/http_flv_output.cpp
    media/http/gb28181_json.cpp
    media/http/http_session.cpp
    media/http/http_server.cpp
    media/gb28181/gb28181_input_media.cpp
    media/gb28181/gb28181_output_media.cpp
    media/gb28181/gb28181_udp_session.cpp
    media/gb28181/gb28181_udp_output_session.cpp
    media/gb28181/gb28181_tcp_output_session.cpp
    media/gb28181/gb28181_tcp_session.cpp
    media/gb28181/gb28181.cpp
    media/webrtc/dtls_certificate.cpp
    media/webrtc/dtls_transport.cpp
    media/webrtc/stun_message.cpp
    media/webrtc/srtp_transport.cpp
    media/webrtc/webrtc_sdp.cpp
    media/webrtc/whep_session.cpp
    media/webrtc/whep.cpp
    media/webrtc/webrtc_output.cpp
    media/rtmp/rtmp_input_session.cpp
    media/rtmp/rtmp_output_session.cpp
    media/rtmp/rtmp_session.cpp
    media/rtmp/rtmp_server.cpp
    media/rtsp/rtsp_sdp.cpp
    media/rtsp/rtsp_uri.cpp
    media/rtsp/rtsp_pull_session.cpp
    media/rtsp/rtsp_input_session.cpp
    media/rtsp/rtsp_input_tcp_session.cpp
    media/rtsp/rtsp_input_udp_session.cpp
    media/rtsp/rtsp_output_session.cpp
    media/rtsp/rtsp_output_tcp_session.cpp
    media/rtsp/rtsp_input_media.cpp
    media/rtsp/rtsp_server.cpp
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

if(BUILD_TESTING)
    add_test(NAME media_server_help COMMAND media_server --help)
    add_test(NAME media_server_invalid_config COMMAND media_server --invalid)
    set_tests_properties(media_server_invalid_config PROPERTIES WILL_FAIL TRUE)
    add_test(NAME media_server_invalid_threads COMMAND media_server --threads 0)
    set_tests_properties(media_server_invalid_threads PROPERTIES WILL_FAIL TRUE)

    add_executable(config_tests
        tests/config_tests.cpp
        config.cpp
    )
    target_include_directories(config_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(config_tests PRIVATE
        media_server_build_options
        media_server_link_options
        Threads::Threads
        ${MEDIA_SERVER_BOOST_PROGRAM_OPTIONS_LIB}
    )
    add_test(NAME config_tests COMMAND config_tests)

    add_executable(service_tests tests/service_tests.cpp service.cpp)
    target_link_libraries(service_tests PRIVATE media_core media_server_link_options)
    add_test(NAME service_tests COMMAND service_tests)

    add_executable(media_pipeline_tests tests/media_pipeline_tests.cpp)
    target_link_libraries(media_pipeline_tests PRIVATE media_core media_server_link_options)
    add_test(NAME media_pipeline_tests COMMAND media_pipeline_tests)

    add_executable(gb28181_udp_peer_tests tests/gb28181_udp_peer_tests.cpp)
    target_link_libraries(gb28181_udp_peer_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_udp_peer_tests COMMAND gb28181_udp_peer_tests)

    add_executable(gb28181_identity_tests tests/gb28181_identity_tests.cpp)
    target_link_libraries(gb28181_identity_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_identity_tests COMMAND gb28181_identity_tests)

    add_executable(gb28181_json_tests tests/gb28181_json_tests.cpp)
    target_link_libraries(gb28181_json_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_json_tests COMMAND gb28181_json_tests)

    add_executable(ireader_rtcp_tests tests/ireader_rtcp_tests.cpp)
    target_link_libraries(ireader_rtcp_tests PRIVATE ireader_media)
    add_test(NAME ireader_rtcp_tests COMMAND ireader_rtcp_tests)

    add_executable(webrtc_signaling_tests tests/webrtc_signaling_tests.cpp)
    target_link_libraries(webrtc_signaling_tests PRIVATE media_core media_server_link_options)
    add_test(NAME webrtc_signaling_tests COMMAND webrtc_signaling_tests)
endif()
