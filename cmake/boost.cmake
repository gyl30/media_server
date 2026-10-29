if(MEDIA_SERVER_SANITIZER STREQUAL "address")
    list(PREPEND CMAKE_PREFIX_PATH ${PROJECT_SOURCE_DIR}/deps/install/asan)
endif()
find_package(Boost 1.89 REQUIRED CONFIG COMPONENTS context program_options)

add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

set(MEDIA_SERVER_BOOST_LIBS Boost::context)

set(MEDIA_SERVER_BOOST_PROGRAM_OPTIONS_LIB Boost::program_options)
