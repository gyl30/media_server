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

include(CTest)
include(cmake/boost.cmake)
include(cmake/ireader.cmake)

add_library(media_core STATIC
    media/core/media_stream.cpp
    media/core/stream_registry.cpp
    media/codec/codec_utils.cpp
    media/net/tcp_connection.cpp
    media/net/tcp_listener.cpp
    media/flv/flv_output_muxer.cpp
    media/hls/hls_output.cpp
    media/hls/hls_service.cpp
    media/http/http_flv_output.cpp
    media/http/http_session.cpp
    media/http/http_server.cpp
    media/webrtc/dtls_certificate.cpp
    media/webrtc/stun_message.cpp
    media/webrtc/webrtc_sdp.cpp
    media/webrtc/whep_session.cpp
    media/webrtc/whep_service.cpp
    media/webrtc/webrtc_output.cpp
    media/rtmp/rtmp_session.cpp
    media/rtmp/rtmp_server.cpp
    media/rtsp/rtsp_input_session.cpp
    media/rtsp/rtsp_output_session.cpp
    media/rtsp/rtsp_server.cpp
)

target_include_directories(media_core PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_include_directories(media_core SYSTEM PUBLIC
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(media_core PUBLIC
    ireader_media
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${MEDIA_SERVER_BOOST_LIBS}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(media_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
endif()

add_executable(media_server main.cpp)
target_link_libraries(media_server PRIVATE media_core)

if(BUILD_TESTING)
    add_executable(media_pipeline_tests tests/media_pipeline_tests.cpp)
    target_link_libraries(media_pipeline_tests PRIVATE media_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(media_pipeline_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()
    add_test(NAME media_pipeline_tests COMMAND media_pipeline_tests)

    add_executable(webrtc_signaling_tests tests/webrtc_signaling_tests.cpp)
    target_link_libraries(webrtc_signaling_tests PRIVATE media_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(webrtc_signaling_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror
        )
    endif()
    add_test(NAME webrtc_signaling_tests COMMAND webrtc_signaling_tests)
endif()
