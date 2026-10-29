#!/bin/bash
# usage: build.sh [ffmpeg-prefix]; builds aacbench (FFmpeg/libfdk direct) and msbench (links media_server's audio_transcoder from build/)
set -eu
cd "$(dirname "$0")"
R=$(git rev-parse --show-toplevel)
F=${1:-$HOME/ffmpeg901}
export PKG_CONFIG_PATH=$F/lib/pkgconfig:$HOME/ffmpeg901/lib/pkgconfig
/usr/bin/g++-16 -O2 -std=c++20 aacbench.cpp -o aacbench $(pkg-config --cflags libavformat) \
    $(pkg-config --libs --static libavformat libavcodec libavutil) -Wl,-Bstatic -lfdk-aac -Wl,-Bdynamic
/usr/bin/g++-16 -O2 -std=c++23 -DFMT_SHARED -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_SHARED_LIB -I$R msbench.cpp -o msbench \
    $(pkg-config --cflags libavformat) $R/build/libmedia_core.a $R/build/libireader_media.a \
    $(pkg-config --libs --static libavformat libavcodec libswresample libavutil) -lspdlog -lfmt
