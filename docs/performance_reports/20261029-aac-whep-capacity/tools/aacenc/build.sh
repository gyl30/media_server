#!/bin/bash
# usage: build.sh <ffmpeg-prefix> <output-binary>
set -eu
export PKG_CONFIG_PATH=$1/lib/pkgconfig
gcc -O2 -std=c11 -D_GNU_SOURCE "$(dirname "$0")/aacenc_check.c" -o "$2" $(pkg-config --cflags libavcodec) $(pkg-config --libs --static libavcodec libavutil) -lm
