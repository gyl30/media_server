include(ExternalProject)
include(ProcessorCount)

ProcessorCount(ffmpeg_jobs)
set(ffmpeg_prefix ${CMAKE_BINARY_DIR}/third-install)
set(ffmpeg_dependency_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_INSTALL_PREFIX=${ffmpeg_prefix}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHARED_LIBS=OFF
)

ExternalProject_Add(opus_external
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/third/opus
    CMAKE_ARGS ${ffmpeg_dependency_args} -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
    BUILD_BYPRODUCTS ${ffmpeg_prefix}/lib/libopus.a
)

ExternalProject_Add(fdk_aac_external
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/third/fdk-aac
    CMAKE_ARGS ${ffmpeg_dependency_args} -DFDK_AAC_INSTALL_CMAKE_CONFIG_MODULE=OFF
    BUILD_BYPRODUCTS ${ffmpeg_prefix}/lib/libfdk-aac.a
)

set(ffmpeg_libraries avformat avcodec swresample avutil)
set(ffmpeg_byproducts)
foreach(library IN LISTS ffmpeg_libraries)
    list(APPEND ffmpeg_byproducts ${ffmpeg_prefix}/lib/lib${library}.a)
endforeach()

ExternalProject_Add(ffmpeg_external
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/third/ffmpeg
    DEPENDS opus_external fdk_aac_external
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${ffmpeg_prefix}/lib/pkgconfig
        <SOURCE_DIR>/configure
        --prefix=${ffmpeg_prefix}
        --cc=${CMAKE_C_COMPILER}
        --pkg-config-flags=--static
        --enable-static --disable-shared --enable-pic
        --disable-programs --disable-doc
        --disable-autodetect --disable-everything
        --disable-avdevice --disable-avfilter --disable-swscale
        --enable-libopus --enable-libfdk-aac
        --enable-decoder=aac,opus
        --enable-encoder=libfdk_aac,libopus,opus
        --enable-parser=aac,opus,h264
        --enable-demuxer=matroska
        --enable-protocol=file
        --enable-swresample
    BUILD_COMMAND make -j${ffmpeg_jobs}
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${ffmpeg_byproducts}
)

foreach(submodule IN ITEMS opus fdk-aac ffmpeg)
    execute_process(COMMAND git -C ${PROJECT_SOURCE_DIR}/third/${submodule} rev-parse --path-format=absolute --git-path HEAD
        OUTPUT_VARIABLE ffmpeg_head_${submodule} OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endforeach()
ExternalProject_Add_StepDependencies(opus_external configure ${ffmpeg_head_opus})
ExternalProject_Add_StepDependencies(fdk_aac_external configure ${ffmpeg_head_fdk-aac})
ExternalProject_Add_StepDependencies(ffmpeg_external configure ${ffmpeg_head_opus} ${ffmpeg_head_fdk-aac} ${ffmpeg_head_ffmpeg})

file(MAKE_DIRECTORY ${ffmpeg_prefix}/include)

add_library(ffmpeg_opus STATIC IMPORTED)
set_target_properties(ffmpeg_opus PROPERTIES IMPORTED_LOCATION ${ffmpeg_prefix}/lib/libopus.a)
add_dependencies(ffmpeg_opus opus_external)

add_library(ffmpeg_fdk_aac STATIC IMPORTED)
set_target_properties(ffmpeg_fdk_aac PROPERTIES IMPORTED_LOCATION ${ffmpeg_prefix}/lib/libfdk-aac.a)
add_dependencies(ffmpeg_fdk_aac fdk_aac_external)

set(ffmpeg_link_libraries_avutil Threads::Threads m)
set(ffmpeg_link_libraries_swresample ffmpeg_avutil m)
set(ffmpeg_link_libraries_avcodec ffmpeg_swresample ffmpeg_avutil ffmpeg_opus ffmpeg_fdk_aac Threads::Threads m)
set(ffmpeg_link_libraries_avformat ffmpeg_avcodec ffmpeg_avutil m)
foreach(library IN LISTS ffmpeg_libraries)
    add_library(ffmpeg_${library} STATIC IMPORTED)
    set_target_properties(ffmpeg_${library} PROPERTIES
        IMPORTED_LOCATION ${ffmpeg_prefix}/lib/lib${library}.a
        INTERFACE_INCLUDE_DIRECTORIES ${ffmpeg_prefix}/include
        INTERFACE_LINK_LIBRARIES "${ffmpeg_link_libraries_${library}}"
    )
    add_dependencies(ffmpeg_${library} ffmpeg_external)
endforeach()

add_library(ffmpeg_audio_dependency INTERFACE)
target_link_libraries(ffmpeg_audio_dependency INTERFACE ffmpeg_avcodec ffmpeg_swresample ffmpeg_avutil)
add_library(FFmpeg::audio ALIAS ffmpeg_audio_dependency)
add_library(FFmpeg::format ALIAS ffmpeg_avformat)
