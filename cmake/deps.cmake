set(media_server_deps_prefix ${PROJECT_SOURCE_DIR}/deps/install)
set(media_server_deps_info ${media_server_deps_prefix}/BUILD-INFO)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/deps/sources.txt
    ${PROJECT_SOURCE_DIR}/deps/build.sh
)
if(NOT EXISTS ${media_server_deps_info})
    message(FATAL_ERROR "${media_server_deps_prefix} is not built; run CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} deps/build.sh")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${media_server_deps_info})

file(READ ${PROJECT_SOURCE_DIR}/deps/sources.txt deps_sources)
file(READ ${PROJECT_SOURCE_DIR}/deps/build.sh deps_script)
string(SHA256 deps_inputs "${deps_sources}${deps_script}")
execute_process(COMMAND ${CMAKE_C_COMPILER} --version OUTPUT_VARIABLE deps_c_version)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} --version OUTPUT_VARIABLE deps_cxx_version)
string(REGEX MATCH "^[^\n]*" deps_c_version "${deps_c_version}")
string(REGEX MATCH "^[^\n]*" deps_cxx_version "${deps_cxx_version}")
file(STRINGS ${media_server_deps_info} deps_info)
if(NOT deps_info STREQUAL "inputs ${deps_inputs};cc ${deps_c_version};cxx ${deps_cxx_version}")
    message(FATAL_ERROR
        "${media_server_deps_prefix} does not match deps/sources.txt, deps/build.sh or the configured compilers; "
        "remove it and run CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} deps/build.sh\n"
        "BUILD-INFO: ${deps_info}")
endif()

list(PREPEND CMAKE_PREFIX_PATH ${media_server_deps_prefix})
set(media_server_boost_context_prefix ${media_server_deps_prefix})
if(MEDIA_SERVER_SANITIZER STREQUAL "address")
    set(media_server_boost_context_prefix ${media_server_deps_prefix}/asan)
elseif(MEDIA_SERVER_SANITIZER STREQUAL "thread")
    set(media_server_boost_context_prefix ${media_server_deps_prefix}/tsan)
endif()
list(PREPEND CMAKE_PREFIX_PATH ${media_server_boost_context_prefix})

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.89 REQUIRED CONFIG COMPONENTS context program_options)
add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

set(deps_paths
    ${OPENSSL_INCLUDE_DIR}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${Boost_DIR}
    ${boost_headers_DIR}
    ${boost_program_options_DIR}
)
if(MEDIA_SERVER_SANITIZER STREQUAL "none")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
    list(APPEND deps_paths ${JEMALLOC_LINK_LIBRARIES})
endif()
cmake_path(IS_PREFIX media_server_boost_context_prefix "${boost_context_DIR}" NORMALIZE deps_inside)
foreach(deps_path IN LISTS deps_paths)
    cmake_path(IS_PREFIX media_server_deps_prefix "${deps_path}" NORMALIZE deps_path_inside)
    if(NOT deps_path_inside)
        set(deps_inside OFF)
    endif()
endforeach()
if(NOT deps_inside)
    message(FATAL_ERROR
        "dependencies must come from ${media_server_deps_prefix} (Boost.Context from ${media_server_boost_context_prefix}); "
        "remove ${CMAKE_BINARY_DIR}/CMakeCache.txt if this build directory was configured with other dependencies\n"
        "found: ${deps_paths};${boost_context_DIR}")
endif()
