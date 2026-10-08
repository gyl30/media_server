set(ENABLE_OPENSSL ON CACHE BOOL "Enable OpenSSL crypto engine" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "Enable MbedTLS crypto engine" FORCE)
set(ENABLE_NSS OFF CACHE BOOL "Enable NSS crypto engine" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "Build libSRTP test applications" FORCE)

set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third/libsrtp EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
