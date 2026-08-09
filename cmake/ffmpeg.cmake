find_package(PkgConfig REQUIRED)

pkg_check_modules(
    FFMPEG_AUDIO
    REQUIRED
    IMPORTED_TARGET
    libavcodec
    libavutil
    libswresample
)

add_library(ffmpeg_audio_dependency INTERFACE)

target_link_libraries(
    ffmpeg_audio_dependency
    INTERFACE
        PkgConfig::FFMPEG_AUDIO
)

add_library(FFmpeg::audio ALIAS ffmpeg_audio_dependency)
