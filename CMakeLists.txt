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
            set(CMAKE_REQUIRED_LIBRARIES "${BOOST_CONTEXT_LIBRARY}")
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
    media/codec/audio_transcoder.cpp
    media/codec/video_transcoder.cpp
    media/codec/codec_utils.cpp
    media/net/worker_context.cpp
    media/net/io_context_pool.cpp
    media/net/port_manager.cpp
    media/net/tcp_yield_transport.cpp
    media/net/udp_yield_transport.cpp
    media/net/tcp_listener.cpp
    media/flv/flv_muxer.cpp
    media/hls/hls_segmenter.cpp
    media/hls/hls.cpp
    media/http/http_flv_streamer.cpp
    media/http/http_flv_session.cpp
    media/http/hls_http_session.cpp
    media/http/gb28181_json.cpp
    media/http/gb28181_http.cpp
    media/http/rtsp_pull_http.cpp
    media/http/signaling_client.cpp
    media/http/whep_http.cpp
    media/http/whip_http.cpp
    media/http/http_session.cpp
    media/http/http_server.cpp
    media/gb28181/gb28181_event.cpp
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
    media/webrtc/whep_event.cpp
    media/webrtc/whip_session.cpp
    media/webrtc/whip.cpp
    media/webrtc/whip_media_receiver.cpp
    media/webrtc/whep_session.cpp
    media/webrtc/whep.cpp
    media/webrtc/webrtc_packetizer.cpp
    media/rtmp/rtmp_event.cpp
    media/rtmp/rtmp_publish_session.cpp
    media/rtmp/rtmp_play_session.cpp
    media/rtmp/rtmp_session.cpp
    media/rtmp/rtmp_server.cpp
    media/rtsp/rtsp_sdp.cpp
    media/rtsp/rtsp_event.cpp
    media/rtsp/rtsp_uri.cpp
    media/rtsp/rtsp_pull_session.cpp
    media/rtsp/rtsp_pull_media.cpp
    media/rtsp/rtsp_publish_session.cpp
    media/rtsp/rtsp_publish_tcp_session.cpp
    media/rtsp/rtsp_publish_udp_session.cpp
    media/rtsp/rtsp_play_session.cpp
    media/rtsp/rtsp_publish_media.cpp
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
        ${MEDIA_SERVER_BOOST_LIBS}
    )
    add_test(NAME config_tests COMMAND config_tests)

    add_executable(signaling_client_tests tests/signaling_client_tests.cpp)
    target_link_libraries(signaling_client_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS
        unconfigured
        registration
        claim
        rejected
        temporary_failure
        network_error
        status_only
        invalid_url
        timeout
        heartbeat_rejection
    )
        add_test(NAME signaling_client_${test_case} COMMAND signaling_client_tests ${test_case})
    endforeach()

    add_executable(service_tests
        tests/service_tests.cpp
        tests/clients/rtsp_test_client.cpp
        service.cpp
    )
    target_link_libraries(service_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS invalid_bind invalid_webrtc registration registration_stop)
        add_test(NAME service_${test_case} COMMAND service_tests ${test_case})
    endforeach()

    add_executable(media_pipeline_tests
        tests/media_pipeline_tests.cpp
        tests/clients/rtmp_test_client.cpp
        tests/clients/rtsp_test_client.cpp
    )
    target_link_libraries(media_pipeline_tests PRIVATE media_core media_server_link_options)
    add_test(NAME media_pipeline_tests COMMAND media_pipeline_tests)
    set_tests_properties(media_pipeline_tests PROPERTIES RUN_SERIAL TRUE)
    foreach(test_case IN ITEMS
        rtmp_claim_invalid
        rtmp_claim_conflict
        rtmp_claim_server_error
        rtmp_claim_timeout
        rtmp_claim_events
        rtmp_claim_disconnect
        rtsp_pull_topology
        rtsp_pull_demux_error
        rtsp_publish_opus_fmtp
        rtsp_claim_network_error
        rtsp_claim_disconnect
        rtsp_claim_accepted
        rtsp_claim_conflict
        rtsp_claim_server_error
        rtsp_claim_timeout
        rtsp_claim_shutdown
        rtsp_server_contract
        rtmp_publish_initial_topology
        rtmp_publish_initial_tracks_timeout
        rtmp_publish_recreate_lifecycle
        rtmp_publish_codec_configuration_updates
        rtmp_publish_rejects_video_codec_change
        rtmp_publish_rejects_audio_codec_change
        rtmp_rejects_live_playback_control
        rtmp_play_pull_codecs_and_order
        rtmp_play_config_reset_and_end
        rtmp_tcp_error_lifecycle
        rtmp_coroutine_publish_client
        rtmp_coroutine_play_client
        rtsp_coroutine_publish_client
        rtsp_coroutine_play_client
        gb28181_tcp_active_connect_successful
        gb28181_tcp_active_connection_refused
        gb28181_multi_sender_identity
        gb28181_receiver_http_parameters
        gb28181_sender_http_parameters
        http_flv_client_disconnect
        http_flv_stream_end_during_write
        http_flv_pending_bootstrap_end
        rtsp_pull_url_contract
        rtsp_pull_establishment_timeout
        rtsp_pull_establishment_progress_timeout
        rtsp_pull_selects_single_audio_and_video
        rtsp_pull_opus_passthrough
        rtsp_pull_rejects_invalid_opus_rate
        rtsp_pull_g711_passthrough
        rtsp_pull_rtp_info_aligns_media_timestamps
        rtsp_publish_rtcp_sender_reports_align_media_timestamps
        rtsp_pull_rejects_mismatched_g711_rtpmap
        rtsp_pull_rejects_audio_only_source
        rtsp_pull_initial_tracks_timeout
        rtsp_pull_independent_keepalive
        rtsp_play_session_contract
        rtsp_play_terminal_failure_quiesces_reader
        rtsp_play_reply_error
        rtsp_play_recreate_lifecycle
        rtsp_play_media_delivery
        rtsp_play_audio_video_order
        rtsp_play_h265
        rtsp_play_av1
        rtsp_play_opus_passthrough_boundaries
        rtsp_play_g711_passthrough
        rtsp_play_setup_track_lifecycle
        rtsp_play_unsetup_audio_update_keeps_video_continuity
        rtsp_play_rejects_stale_description
        stream_registry_generation_lifecycle
        stream_registry_session_identity_lifecycle
        hls_module_lifecycle
        rtsp_input_output_boundaries
        rtmp_duplicate_publisher_preserves_source
        rtmp_input_output_boundaries
        rtmp_publish_media_input_error
        whip_media_receiver
        whip_hls_output
        whip_rtmp_output
    )
        add_test(NAME media_pipeline_${test_case} COMMAND media_pipeline_tests ${test_case})
        set_tests_properties(media_pipeline_${test_case} PROPERTIES RUN_SERIAL TRUE)
    endforeach()

    add_executable(rtsp_server_connection_tests tests/rtsp_server_connection_tests.cpp)
    target_link_libraries(rtsp_server_connection_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS idle_timeout control_refreshes_timeout udp_setup_internal_failure record_internal_failure)
        add_test(NAME rtsp_server_connection_${test_case} COMMAND rtsp_server_connection_tests ${test_case})
    endforeach()

    add_executable(rtsp_publish_udp_scheduler_tests tests/rtsp_publish_udp_scheduler_tests.cpp)
    target_link_libraries(rtsp_publish_udp_scheduler_tests PRIVATE media_core media_server_link_options)
    add_test(NAME rtsp_publish_udp_scheduler_tests COMMAND rtsp_publish_udp_scheduler_tests)

    add_executable(rtsp_write_backpressure_tests tests/rtsp_write_backpressure_tests.cpp)
    target_link_libraries(rtsp_write_backpressure_tests PRIVATE media_core media_server_link_options)
    add_test(NAME rtsp_write_backpressure_tests COMMAND rtsp_write_backpressure_tests)

    add_executable(rtmp_write_backpressure_tests tests/rtmp_write_backpressure_tests.cpp)
    target_link_libraries(rtmp_write_backpressure_tests PRIVATE media_core media_server_link_options)
    add_test(NAME rtmp_write_backpressure_tests COMMAND rtmp_write_backpressure_tests)

    add_executable(tcp_yield_transport_tests tests/tcp_yield_transport_tests.cpp)
    target_link_libraries(tcp_yield_transport_tests PRIVATE media_core media_server_link_options)
    add_test(NAME tcp_yield_transport_tests COMMAND tcp_yield_transport_tests)

    add_executable(udp_yield_transport_tests tests/udp_yield_transport_tests.cpp)
    target_link_libraries(udp_yield_transport_tests PRIVATE media_core media_server_link_options)
    add_test(NAME udp_yield_transport_tests COMMAND udp_yield_transport_tests)

    add_executable(gb28181_udp_peer_tests tests/gb28181_udp_peer_tests.cpp)
    target_link_libraries(gb28181_udp_peer_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_udp_peer_tests COMMAND gb28181_udp_peer_tests)

    add_executable(gb28181_udp_sender_tests tests/gb28181_udp_sender_tests.cpp)
    target_link_libraries(gb28181_udp_sender_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS sends_rtp queue_overflow rtcp_shutdown)
        foreach(iteration RANGE 1 10)
            add_test(NAME gb28181_udp_sender_${test_case}_${iteration} COMMAND gb28181_udp_sender_tests ${test_case})
            set_tests_properties(gb28181_udp_sender_${test_case}_${iteration} PROPERTIES RUN_SERIAL TRUE)
        endforeach()
    endforeach()

    add_executable(gb28181_tcp_sender_backpressure_tests tests/gb28181_tcp_sender_backpressure_tests.cpp)
    target_link_libraries(gb28181_tcp_sender_backpressure_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_tcp_sender_backpressure_tests COMMAND gb28181_tcp_sender_backpressure_tests)

    add_executable(gb28181_rtp_fixture tests/gb28181_rtp_fixture.cpp)
    target_link_libraries(gb28181_rtp_fixture PRIVATE media_core media_server_link_options)

    add_executable(gb28181_identity_tests tests/gb28181_identity_tests.cpp)
    target_link_libraries(gb28181_identity_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_identity_tests COMMAND gb28181_identity_tests)

    add_executable(gb28181_json_tests tests/gb28181_json_tests.cpp)
    target_link_libraries(gb28181_json_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_json_tests COMMAND gb28181_json_tests)

    add_executable(gb28181_http_tests tests/gb28181_http_tests.cpp)
    target_link_libraries(gb28181_http_tests PRIVATE media_core media_server_link_options)
    add_test(NAME gb28181_http_tests COMMAND gb28181_http_tests)

    add_executable(rtsp_pull_http_tests tests/rtsp_pull_http_tests.cpp)
    target_link_libraries(rtsp_pull_http_tests PRIVATE media_core media_server_link_options)
    add_test(NAME rtsp_pull_http_tests COMMAND rtsp_pull_http_tests)

    add_executable(runtime_event_tests tests/runtime_event_tests.cpp)
    target_link_libraries(runtime_event_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS strings creation failure ordinary_shutdown)
        add_test(NAME runtime_event_${test_case} COMMAND runtime_event_tests ${test_case})
    endforeach()

    add_executable(signaling_event_tests tests/signaling_event_tests.cpp)
    target_link_libraries(signaling_event_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS
        unconfigured
        heartbeat_failure
        batch
        retry
        retry_order
        retry_overflow
        capacity
        overflow
        nonblocking
        gb_sender_streaming
    )
        add_test(NAME signaling_event_${test_case} COMMAND signaling_event_tests ${test_case})
    endforeach()

    add_executable(port_manager_tests tests/port_manager_tests.cpp)
    target_link_libraries(port_manager_tests PRIVATE media_core media_server_link_options)
    foreach(test_case IN ITEMS
        single_reservation
        pair_reservation
        single_exhaustion
        pair_exhaustion
        concurrent_reservation
        udp_sender_shutdown_release
        udp_sender_bind_failure_release
        udp_receiver_bind_failure_release
        udp_receiver_address_failure_release
    )
        add_test(NAME port_manager_${test_case} COMMAND port_manager_tests ${test_case})
    endforeach()

    add_executable(ireader_rtcp_tests tests/ireader_rtcp_tests.cpp)
    target_link_libraries(ireader_rtcp_tests PRIVATE
        media_server_link_options
        ireader_media
    )
    add_test(NAME ireader_rtcp_tests COMMAND ireader_rtcp_tests)

    add_executable(webrtc_signaling_tests tests/webrtc_signaling_tests.cpp)
    target_link_libraries(webrtc_signaling_tests PRIVATE media_core media_server_link_options)
    add_test(NAME webrtc_signaling_tests COMMAND webrtc_signaling_tests)
    set_tests_properties(webrtc_signaling_tests PROPERTIES RUN_SERIAL TRUE)
    foreach(test_case IN ITEMS
        http_head_response_contract
        http_method_contract
        whep_http_cors
        whip_http_lifecycle
        webrtc_module_shutdown
        whip_http_self_shutdown_releases_reservation
        whep_session_lifecycle
        whep_opus_source_session_lifecycle
        whip_session_ingest
        whip_h265_session_ingest
    )
        add_test(NAME webrtc_signaling_${test_case} COMMAND webrtc_signaling_tests ${test_case})
        set_tests_properties(webrtc_signaling_${test_case} PROPERTIES RUN_SERIAL TRUE)
    endforeach()
endif()
