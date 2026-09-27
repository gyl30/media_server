find_package(PkgConfig REQUIRED)

# 复用构建目录切换加密后端时，旧的 pkg-config 结果和库路径不能继续参与链接。
unset(__pkg_config_checked_SRTP CACHE)
unset(pkgcfg_lib_SRTP_srtp2 CACHE)

pkg_check_modules(
    SRTP
    REQUIRED
    IMPORTED_TARGET
    libsrtp2>=2.7.0
)

if(NOT "crypto" IN_LIST SRTP_STATIC_LIBRARIES)
    message(FATAL_ERROR "libSRTP must use the OpenSSL crypto backend (libsrtp2.pc Libs.private must include -lcrypto)")
endif()

add_library(srtp_dependency INTERFACE)

target_link_libraries(
    srtp_dependency
    INTERFACE
        PkgConfig::SRTP
)

add_library(libSRTP::srtp2 ALIAS srtp_dependency)
