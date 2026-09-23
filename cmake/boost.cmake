find_package(Boost 1.89 REQUIRED CONFIG COMPONENTS context url json program_options)

add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

set(MEDIA_SERVER_BOOST_LIBS Boost::url Boost::json Boost::context)

set(MEDIA_SERVER_BOOST_PROGRAM_OPTIONS_LIB Boost::program_options)
