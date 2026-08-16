set(IREADER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third/ireader)

set(IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/libmpeg/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/libflv/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/libmov/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtp/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtp/source/payload IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/libhttp/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/avbsf/src IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtsp/source IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtsp/source/sdp IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtsp/source/utils IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtsp/source/client IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtsp/source/server IREADER_SOURCES)
aux_source_directory(${IREADER_ROOT}/librtmp/source IREADER_SOURCES)

add_library(ireader_media STATIC ${IREADER_SOURCES})
target_include_directories(ireader_media SYSTEM PUBLIC
    ${IREADER_ROOT}/libmpeg/include
    ${IREADER_ROOT}/libflv/include
    ${IREADER_ROOT}/libmov/include
    ${IREADER_ROOT}/libmkv/include
    ${IREADER_ROOT}/librtp/include
    ${IREADER_ROOT}/libhttp/include
    ${IREADER_ROOT}/avbsf/include
    ${IREADER_ROOT}/librtsp/include
    ${IREADER_ROOT}/librtsp/source/server
    ${IREADER_ROOT}/librtsp/source/utils
    ${IREADER_ROOT}/librtmp/include
)
target_compile_definitions(ireader_media PRIVATE
    NDEBUG
    MPEG_H26X_VERIFY
    MPEG_GUESS_STREAM
    MPEG_ZERO_PAYLOAD_LENGTH
    RTP_MPEG4_GENERIC_SKIP_ADTS
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ireader_media PRIVATE
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Wno-format-truncation
        -Wno-unused-variable
        -Wno-enum-conversion
        -Wno-implicit-function-declaration
        -Wno-pragma-messages
        -Wno-switch
        -Wno-misleading-indentation
        -Wno-unused-value
        -Wno-stringop-overflow
        -Wno-incompatible-pointer-types
        -Wno-comment
    )
endif()
