find_package(PkgConfig REQUIRED)

pkg_check_modules(
    SRTP
    REQUIRED
    IMPORTED_TARGET
    libsrtp2
)

add_library(srtp_dependency INTERFACE)

target_link_libraries(
    srtp_dependency
    INTERFACE
        PkgConfig::SRTP
)

add_library(libSRTP::srtp2 ALIAS srtp_dependency)
